#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#define MAX_SESSOES 10
#define MAX_FILA 20
#define MAX_NOME 50
#define MAX_LINHA 128
#define MAX_DEMANDA_KW 200.0f
#define LIMITE_THROTTLE 0.80f
#define LIMITE_FILA 0.95f
#define TAXA_FIXA 2.50f
#define MULTA_OCIOSIDADE 5.00f
#define OCPP_VERSAO "OCPP 1.6J"
#define CAP_BATERIA_KWH 60.0f
#define MAX_HISTORICO 500

#define CRIT_ID 1
#define CRIT_ENERGIA 2
#define CRIT_CUSTO 3
#define CRIT_TEMPO 4

typedef enum {
    TIPO_AC_LENTO = 1,
    TIPO_AC_RAPIDO = 2,
    TIPO_DC_ULTRA = 3
} TipoCarregador;

typedef enum {
    SESS_INATIVA = 0,
    SESS_ATIVA = 1,
    SESS_THROTTLE = 2,
    SESS_CONCLUIDA = 3,
    SESS_AGUARDANDO = 4
} StatusSessao;

typedef struct {
    int id, horaInicio, tempoConectado, tempoEstimado, minutosExtras;
    char usuario[MAX_NOME], ocppTxId[20];
    TipoCarregador tipo;
    StatusSessao status;
    float bateriaInicial, bateriaAtual, kwhConsumido, potenciaAtual, tarifaKwh, valorTotal;
} Sessao;

typedef struct {
    char usuario[MAX_NOME];
    TipoCarregador tipo;
    float bateriaInicial;
    int horaInicio;
    int tempoConectado;
} ItemFila;

static Sessao sessoes[MAX_SESSOES];
static ItemFila fila[MAX_FILA];
static int tamFila = 0;
static int totalSessoes = 0;
static int proximoId = 1;

static Sessao historico[MAX_HISTORICO];
static int totalHistorico = 0;

static float receitaTotal = 0.0f;
static float kwhTotal = 0.0f;
static int sessoesFinalizadas = 0;
static int tempoTotalMin = 0;

float demandaTotal(void);
void aplicarControleDemanda(int verboso);
void promoverDaFila(void);
void registrarHistorico(const Sessao *s);
void atualizarHistorico(const Sessao *s);
void removerDoHistorico(int id);
void sincronizarHistorico(void);

static void descartarResto(void) {
    int c;
    while ((c = getchar()) != '\n' && c != EOF)
        ;
}

static int lerLinha(char *buf, int tam) {
    if (fgets(buf, tam, stdin) == NULL)
        return 0;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n')
        buf[len - 1] = '\0';
    else
        descartarResto();
    return 1;
}

static void aparar(char *s) {
    int i = 0;
    size_t j;
    while (isspace((unsigned char)s[i]))
        i++;
    if (i > 0)
        memmove(s, s + i, strlen(s + i) + 1);
    j = strlen(s);
    while (j > 0 && isspace((unsigned char)s[j - 1]))
        s[--j] = '\0';
}

static int lerInteiro(const char *prompt, int min, int max, int *out) {
    char linha[MAX_LINHA];
    char *fim;
    long v;
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (!lerLinha(linha, sizeof linha))
            return 0;
        aparar(linha);
        if (linha[0] == '\0') {
            printf("  [!] Entrada vazia. Informe um numero inteiro entre %d e %d.\n", min, max);
            continue;
        }
        errno = 0;
        v = strtol(linha, &fim, 10);
        while (*fim == ' ' || *fim == '\t')
            fim++;
        if (fim == linha || *fim != '\0' || errno == ERANGE) {
            printf("  [!] Valor invalido ('%s'). Digite apenas numeros inteiros.\n", linha);
            continue;
        }
        if (v < min || v > max) {
            printf("  [!] Fora do intervalo permitido (%d a %d).\n", min, max);
            continue;
        }
        *out = (int)v;
        return 1;
    }
}

static int lerFloat(const char *prompt, float min, float max, float *out) {
    char linha[MAX_LINHA];
    char *fim, *p;
    double v;
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (!lerLinha(linha, sizeof linha))
            return 0;
        aparar(linha);
        if (linha[0] == '\0') {
            printf("  [!] Entrada vazia. Informe um numero entre %.1f e %.1f.\n", min, max);
            continue;
        }
        for (p = linha; *p; p++)
            if (*p == ',')
                *p = '.'; 
        errno = 0;
        v = strtod(linha, &fim);
        while (*fim == ' ' || *fim == '\t')
            fim++;
        if (fim == linha || *fim != '\0' || errno == ERANGE) {
            printf("  [!] Valor invalido ('%s'). Digite apenas numeros.\n", linha);
            continue;
        }
        if (v < min || v > max) {
            printf("  [!] Fora do intervalo permitido (%.1f a %.1f).\n", min, max);
            continue;
        }
        *out = (float)v;
        return 1;
    }
}

static int lerHorario(const char *prompt, int *totalMin) {
    char linha[MAX_LINHA];
    char *fim;
    long h, m;
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (!lerLinha(linha, sizeof linha))
            return 0;
        aparar(linha);
        if (linha[0] == '\0') {
            printf("  [!] Horario vazio. Use HH ou HH:MM (ex.: 14 ou 14:30).\n");
            continue;
        }
        errno = 0;
        h = strtol(linha, &fim, 10);
        m = 0;
        if (fim == linha || errno == ERANGE) {
            printf("  [!] Horario invalido ('%s'). Use HH ou HH:MM.\n", linha);
            continue;
        }
        if (*fim == ':' || *fim == 'h' || *fim == 'H') {
            char *ini = fim + 1;
            m = strtol(ini, &fim, 10);
            if (fim == ini) {
                printf("  [!] Minutos invalidos. Use HH:MM (ex.: 14:30).\n");
                continue;
            }
        }
        while (*fim == ' ' || *fim == '\t')
            fim++;
        if (*fim != '\0') {
            printf("  [!] Horario invalido ('%s'). Use HH ou HH:MM.\n", linha);
            continue;
        }
        if (h < 0 || h > 23 || m < 0 || m > 59) {
            printf("  [!] Horario fora do intervalo (00:00 a 23:59).\n");
            continue;
        }
        *totalMin = (int)(h * 60 + m);
        return 1;
    }
}

static int lerNome(const char *prompt, char *dest, int tam) {
    char linha[MAX_LINHA];
    int i, valido;
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (!lerLinha(linha, sizeof linha))
            return 0;
        aparar(linha);
        valido = (linha[0] != '\0');
        for (i = 0; valido && linha[i] != '\0'; i++)
            if (isdigit((unsigned char)linha[i]))
                valido = 0;
        if (!valido) {
            printf("  [!] Nome invalido (nao pode ser vazio nem conter numeros).\n");
            continue;
        }
        strncpy(dest, linha, (size_t)tam - 1);
        dest[tam - 1] = '\0';
        return 1;
    }
}

static int confirmar(const char *prompt) {
    int r;
    if (!lerInteiro(prompt, 0, 1, &r))
        return 0;
    return r == 1;
}

void limparTela(void) {
#ifdef _WIN32
    system("cls");
#else
    printf("\033[2J\033[H");
#endif
}

void pausar(void) {
    int c;
    printf("\n  Pressione ENTER para continuar...");
    fflush(stdout);
    while ((c = getchar()) != '\n' && c != EOF)
        ;
}

void barra(const char *label, float pct, int width) {
    int filled;
    if (pct < 0.0f)
        pct = 0.0f;
    filled = (int)(pct / 100.0f * width);
    if (filled > width)
        filled = width;
    printf("  %-16s [", label);
    for (int i = 0; i < width; i++)
        printf(i < filled ? "#" : ".");
    printf("] %5.1f%%\n", pct);
}

void spinner(const char *msg, int ciclos) {
    const char frames[] = "|/-\\";
    printf("  %s ", msg);
    fflush(stdout);
    for (int i = 0; i < ciclos; i++) {
        printf("\b%c", frames[i % 4]);
        fflush(stdout);
        for (volatile long j = 0; j < 6000000L; j++)
            ;
    }
    printf("\b[OK]\n");
}

void cabecalho(const char *titulo) {
    printf("\n");
    printf("  +================================================+\n");
    printf("  |        * CHARGEGRID INTELLIGENCE *             |\n");
    printf("  +================================================+\n");
    printf("  |  %-46s|\n", titulo);
    printf("  +================================================+\n\n");
}

void sep(void) { printf("  ------------------------------------------------\n"); }
void sepTabela(void) {
    printf("  ----------------------------------------------------------------------------------------\n");
}
void sep2(void) { printf("  ================================================\n"); }

float potenciaBase(TipoCarregador t) {
    switch (t) {
    case TIPO_AC_LENTO:
        return 7.0f;
    case TIPO_AC_RAPIDO:
        return 22.0f;
    case TIPO_DC_ULTRA:
        return 50.0f;
    default:
        return 7.0f;
    }
}

const char *nomeTipo(TipoCarregador t) {
    switch (t) {
    case TIPO_AC_LENTO:
        return "AC Lento       ( 7 kW)";
    case TIPO_AC_RAPIDO:
        return "AC Semirrapido (22 kW)";
    case TIPO_DC_ULTRA:
        return "DC Rapido      (50 kW)";
    default:
        return "Desconhecido          ";
    }
}

const char *nomeStatus(StatusSessao s) {
    switch (s) {
    case SESS_INATIVA:
        return "Inativa   ";
    case SESS_ATIVA:
        return "Ativa     ";
    case SESS_THROTTLE:
        return "Throttle  ";
    case SESS_CONCLUIDA:
        return "Concluida ";
    case SESS_AGUARDANDO:
        return "Aguardando";
    default:
        return "?         ";
    }
}

float calcTarifa(TipoCarregador tipo, int horaMin) {
    float base;
    switch (tipo) {
    case TIPO_AC_LENTO:
        base = 3.50f;
        break;
    case TIPO_AC_RAPIDO:
        base = 4.20f;
        break;
    case TIPO_DC_ULTRA:
        base = 5.20f;
        break;
    default:
        base = 3.50f;
    }
    if (horaMin >= 1020 && horaMin < 1320)
        base *= 1.20f;
    else if (horaMin < 360)
        base *= 0.85f;
    if (demandaTotal() / MAX_DEMANDA_KW > LIMITE_THROTTLE)
        base *= 1.10f;
    return base;
}

int estimarTempo(float batInicial, float potKw) {
    float bat = batInicial;
    int min = 0;
    float taxaR = (potKw / CAP_BATERIA_KWH * 100.0f) / 60.0f;
    float taxaL = taxaR * 0.45f;
    while (bat < 100.0f) {
        bat += (bat < 80.0f) ? taxaR : taxaL;
        if (bat > 100.0f)
            bat = 100.0f;
        min++;
        if (min > 9999)
            break;
    }
    return min;
}

void gerarTxId(char *buf, int id) {
    sprintf(buf, "TXN-%04d-%05d", id, (id * 13337) % 99999);
}

float demandaTotal(void) {
    float total = 0.0f;
    for (int i = 0; i < MAX_SESSOES; i++)
        if (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE)
            total += sessoes[i].potenciaAtual;
    return total;
}

int contarSessoesEmCarga(void) {
    int n = 0;
    for (int i = 0; i < MAX_SESSOES; i++)
        if (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE)
            n++;
    return n;
}

void aplicarControleDemanda(int verboso) {
    float dem = demandaTotal();
    float ratio = dem / MAX_DEMANDA_KW;

    if (ratio <= LIMITE_THROTTLE) {
        int restaurou = 0;
        for (int i = 0; i < MAX_SESSOES; i++) {
            if (sessoes[i].status == SESS_THROTTLE) {
                sessoes[i].potenciaAtual = potenciaBase(sessoes[i].tipo);
                sessoes[i].status = SESS_ATIVA;
                sessoes[i].tempoEstimado = estimarTempo(sessoes[i].bateriaAtual, sessoes[i].potenciaAtual);
                atualizarHistorico(&sessoes[i]);
                printf("  [^] Sessao #%d (%s): throttle removido -> %.1f kW\n",
                       sessoes[i].id, sessoes[i].usuario, sessoes[i].potenciaAtual);
                restaurou = 1;
            }
        }
        if (restaurou)
            printf("\n");
        else if (verboso)
            printf("  [OK] Demanda dentro do limite (%.1f%% de %.0f%%). "
                   "Nenhuma limitacao necessaria.\n\n",
                   ratio * 100.0f, LIMITE_THROTTLE * 100.0f);
        return;
    }

    float fator = (ratio >= LIMITE_FILA) ? 0.50f : 0.75f;
    printf("\n  [!] CONTROLE DE DEMANDA ATIVADO\n");
    printf("  Demanda: %.1f kW / %.0f kW (%.0f%%)  ->  potencia reduzida a %.0f%%\n\n",
           dem, MAX_DEMANDA_KW, ratio * 100.0f, fator * 100.0f);

    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE) {
            float nova = potenciaBase(sessoes[i].tipo) * fator;
            sessoes[i].potenciaAtual = nova;
            sessoes[i].status = SESS_THROTTLE;
            sessoes[i].tempoEstimado = estimarTempo(sessoes[i].bateriaAtual, nova);
            atualizarHistorico(&sessoes[i]);
            printf("  [v] Sessao #%d (%s): %.1f kW -> %.1f kW\n",
                   sessoes[i].id, sessoes[i].usuario,
                   potenciaBase(sessoes[i].tipo), nova);
        }
    }
    printf("\n");
}

void painelDemanda(void) {
    float dem = demandaTotal();
    float pct = dem / MAX_DEMANDA_KW * 100.0f;
    int emCarga = contarSessoesEmCarga();

    cabecalho("CONTROLE DE DEMANDA");

    printf("  Sessoes em carga  : %d\n", emCarga);
    printf("  Demanda atual     : %.1f kW de %.0f kW disponiveis\n", dem, MAX_DEMANDA_KW);
    printf("  Margem livre      : %.1f kW\n\n", MAX_DEMANDA_KW - dem);
    barra("Utilizacao Grid", pct, 25);
    printf("\n  Limite de throttle: %.0f%%   |   Limite para fila: %.0f%%\n",
           LIMITE_THROTTLE * 100.0f, LIMITE_FILA * 100.0f);
    sep();

    if (emCarga == 0) {
        printf("  Nenhuma sessao em carga no momento, portanto a demanda e 0.0 kW.\n");
        printf("  Abra uma sessao (opcao 1) para ver o controle de demanda em acao.\n");
        pausar();
        return;
    }

    printf("  Potencia por sessao:\n");
    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE)
            printf("    #%-3d %-20s %-19s %5.1f kW  (nominal %.1f kW)\n",
                   sessoes[i].id, sessoes[i].usuario, nomeStatus(sessoes[i].status),
                   sessoes[i].potenciaAtual, potenciaBase(sessoes[i].tipo));
    }
    printf("\n");
    aplicarControleDemanda(1);
    pausar();
}

int posicaoNaFila(const char *usuario) {
    for (int i = 0; i < tamFila; i++)
        if (strcmp(fila[i].usuario, usuario) == 0)
            return i + 1;
    return 0;
}

void exibirFila(void) {
    if (tamFila == 0) {
        printf("  Fila de espera vazia.\n");
        return;
    }
    printf("  +--  FILA DE ESPERA (%d aguardando)  -----------+\n", tamFila);
    for (int i = 0; i < tamFila; i++) {
        printf("  | Pos %-2d  %-20s  %s |\n",
               i + 1, fila[i].usuario, nomeTipo(fila[i].tipo));
    }
    printf("  +------------------------------------------------+\n");
}

void promoverDaFila(void) {
    if (tamFila == 0)
        return;

    int slotLivre = -1;
    for (int i = 0; i < MAX_SESSOES; i++)
        if (sessoes[i].status == SESS_INATIVA || sessoes[i].status == SESS_CONCLUIDA) {
            slotLivre = i;
            break;
        }
    if (slotLivre < 0)
        return;

    float dem = demandaTotal();
    if (dem / MAX_DEMANDA_KW >= LIMITE_FILA)
        return;

    ItemFila prom = fila[0];
    for (int i = 0; i < tamFila - 1; i++)
        fila[i] = fila[i + 1];
    tamFila--;

    Sessao *s = &sessoes[slotLivre];
    memset(s, 0, sizeof(Sessao));
    s->id = proximoId++;
    strncpy(s->usuario, prom.usuario, MAX_NOME - 1);
    s->usuario[MAX_NOME - 1] = '\0';
    s->tipo = prom.tipo;
    s->bateriaInicial = prom.bateriaInicial;
    s->bateriaAtual = prom.bateriaInicial;
    s->horaInicio = prom.horaInicio;
    s->tempoConectado = prom.tempoConectado;
    s->potenciaAtual = potenciaBase(prom.tipo);
    s->tarifaKwh = calcTarifa(prom.tipo, prom.horaInicio);
    s->tempoEstimado = estimarTempo(prom.bateriaInicial, s->potenciaAtual);
    gerarTxId(s->ocppTxId, s->id);
    s->status = SESS_ATIVA;
    totalSessoes++;
    registrarHistorico(s);

    printf("\n  [>>] FILA: '%s' promovido(a) -> Sessao #%d iniciada!\n",
           s->usuario, s->id);
    printf("  Posicoes restantes na fila: %d\n", tamFila);
}

void adicionarNaFila(const char *usuario, TipoCarregador tipo, float batInicial,
                     int horaInicio, int tempoConectado) {
    if (tamFila >= MAX_FILA) {
        printf("  [X] Fila lotada! Tente novamente mais tarde.\n");
        return;
    }
    ItemFila *item = &fila[tamFila++];
    strncpy(item->usuario, usuario, MAX_NOME - 1);
    item->usuario[MAX_NOME - 1] = '\0';
    item->tipo = tipo;
    item->bateriaInicial = batInicial;
    item->horaInicio = horaInicio;
    item->tempoConectado = tempoConectado;

    printf("\n  [!] Demanda no limite! Sessao adicionada a fila.\n");
    printf("  Posicao: %d de %d\n", posicaoNaFila(usuario), tamFila);
    printf("  Voce sera promovido automaticamente quando houver capacidade.\n");
}

void registrarHistorico(const Sessao *s) {
    if (totalHistorico >= MAX_HISTORICO) {
        printf("  [!] Historico cheio (%d registros). Sessao nao registrada.\n", MAX_HISTORICO);
        return;
    }
    historico[totalHistorico] = *s;
    totalHistorico++;
}

void atualizarHistorico(const Sessao *s) {
    for (int i = 0; i < totalHistorico; i++) {
        if (historico[i].id == s->id) {
            historico[i] = *s;
            return;
        }
    }
    registrarHistorico(s);
}

void removerDoHistorico(int id) {
    for (int i = 0; i < totalHistorico; i++) {
        if (historico[i].id == id) {
            for (int j = i; j < totalHistorico - 1; j++)
                historico[j] = historico[j + 1];
            totalHistorico--;
            return;
        }
    }
}

void sincronizarHistorico(void) {
    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status == SESS_INATIVA)
            continue;
        atualizarHistorico(&sessoes[i]);
    }
}

int contarPendentes(void) {
    int n = 0;
    for (int i = 0; i < totalHistorico; i++)
        if (historico[i].status != SESS_CONCLUIDA)
            n++;
    return n;
}

int contarConcluidas(void) {
    int n = 0;
    for (int i = 0; i < totalHistorico; i++)
        if (historico[i].status == SESS_CONCLUIDA)
            n++;
    return n;
}

void avisarPendentes(void) {
    int n = contarPendentes();
    if (n == 0)
        return;
    printf("  [!] ATENCAO: %d sessao(oes) ainda em andamento.\n", n);
    printf("      Energia e custo so ficam DEFINITIVOS depois de simular a\n");
    printf("      sessao (opcao 7) ou encerra-la (opcao 12).\n");
    printf("      Ate la, os valores abaixo sao parciais.\n\n");
}

void imprimirCabecalhoHistorico(void) {
    printf("  %-4s %-20s %-23s %-11s %10s %9s %11s\n",
           "ID", "Usuario", "Tipo", "Status", "Energia", "Tempo", "Custo");
    sepTabela();
}

void imprimirLinhaHistorico(const Sessao *s) {
    printf("  #%-3d %-20s %-23s %-11s %6.2f kWh %5d min R$ %8.2f\n",
           s->id, s->usuario, nomeTipo(s->tipo), nomeStatus(s->status),
           s->kwhConsumido, s->tempoConectado, s->valorTotal);
}

void listarSessoes(void) {
    sincronizarHistorico();
    cabecalho("LISTAGEM DE SESSOES (HISTORICO)");

    if (totalHistorico == 0) {
        printf("  Nenhuma sessao registrada ainda.\n");
        printf("  Use a opcao [1] para abrir a primeira sessao.\n");
        pausar();
        return;
    }

    avisarPendentes();
    imprimirCabecalhoHistorico();
    for (int i = 0; i < totalHistorico; i++)
        imprimirLinhaHistorico(&historico[i]);
    sepTabela();
    printf("  Total registrado: %d  |  Concluidas: %d  |  Em andamento: %d\n",
           totalHistorico, contarConcluidas(), contarPendentes());
    if (tamFila > 0)
        printf("  Obs.: %d usuario(s) na fila ainda nao geraram sessao.\n", tamFila);
    pausar();
}

int buscaLinearPorId(int id, int *comparacoes) {
    *comparacoes = 0;
    for (int i = 0; i < totalHistorico; i++) {
        (*comparacoes)++;
        if (historico[i].id == id)
            return i;
    }
    return -1;
}

int buscaLinearPorNome(const char *alvo, int inicio, int *comparacoes) {
    char a[MAX_NOME], b[MAX_NOME];
    int k;
    for (k = 0; alvo[k] && k < MAX_NOME - 1; k++)
        a[k] = (char)tolower((unsigned char)alvo[k]);
    a[k] = '\0';

    for (int i = inicio; i < totalHistorico; i++) {
        (*comparacoes)++;
        for (k = 0; historico[i].usuario[k] && k < MAX_NOME - 1; k++)
            b[k] = (char)tolower((unsigned char)historico[i].usuario[k]);
        b[k] = '\0';
        if (strstr(b, a) != NULL)
            return i;
    }
    return -1;
}

void bubbleSortHistorico(int criterio);

int buscaBinariaPorId(int id, int *comparacoes) {
    int inicio = 0, fim = totalHistorico - 1;
    *comparacoes = 0;
    while (inicio <= fim) {
        int meio = inicio + (fim - inicio) / 2;
        (*comparacoes)++;
        if (historico[meio].id == id)
            return meio;
        if (historico[meio].id < id)
            inicio = meio + 1;
        else
            fim = meio - 1;
    }
    return -1;
}

void buscarSessao(void) {
    sincronizarHistorico();
    cabecalho("BUSCAR SESSAO");

    if (totalHistorico == 0) {
        printf("  Nenhuma sessao registrada ainda.\n");
        pausar();
        return;
    }

    avisarPendentes();

    printf("  Tipo de busca:\n");
    printf("    [1] Por ID  - busca linear    O(n)\n");
    printf("    [2] Por ID  - busca binaria   O(log n)  (ordena por ID antes)\n");
    printf("    [3] Por nome do usuario - busca linear O(n)\n");
    printf("    [0] Voltar\n");

    int modo;
    if (!lerInteiro("  Escolha: ", 0, 3, &modo) || modo == 0)
        return;

    int comparacoes = 0, idx = -1;

    if (modo == 3) {
        char nome[MAX_NOME];
        if (!lerNome("  Nome (ou parte dele): ", nome, MAX_NOME))
            return;
        idx = buscaLinearPorNome(nome, 0, &comparacoes);
        if (idx == -1) {
            printf("\n  [!] Nenhuma sessao encontrada para '%s'. (%d comparacoes)\n",
                   nome, comparacoes);
        } else {
            printf("\n  Resultado(s) para '%s':\n\n", nome);
            imprimirCabecalhoHistorico();
            while (idx != -1) {
                imprimirLinhaHistorico(&historico[idx]);
                idx = buscaLinearPorNome(nome, idx + 1, &comparacoes);
            }
            sepTabela();
            printf("  Comparacoes realizadas: %d\n", comparacoes);
        }
        pausar();
        return;
    }

    int id;
    if (!lerInteiro("  Digite o ID da sessao: ", 1, 999999, &id))
        return;

    if (modo == 2) {
        bubbleSortHistorico(CRIT_ID);
        printf("\n  (Historico ordenado por ID para permitir a busca binaria.)\n");
        idx = buscaBinariaPorId(id, &comparacoes);
    } else {
        idx = buscaLinearPorId(id, &comparacoes);
    }

    if (idx == -1) {
        printf("\n  [!] Sessao com ID %d nao encontrada. (%d comparacoes)\n", id, comparacoes);
    } else {
        Sessao *s = &historico[idx];
        printf("\n  Sessao encontrada em %d comparacao(oes):\n\n", comparacoes);
        imprimirCabecalhoHistorico();
        imprimirLinhaHistorico(s);
        sepTabela();
        printf("  Transacao OCPP : %s\n", s->ocppTxId);
        printf("  Bateria        : %.1f%% -> %.1f%%\n", s->bateriaInicial, s->bateriaAtual);
        printf("  Tarifa         : R$ %.2f/kWh\n", s->tarifaKwh);
        printf("  Potencia atual : %.1f kW\n", s->potenciaAtual);
        if (s->status != SESS_CONCLUIDA)
            printf("  [i] Sessao em andamento: energia e custo ainda parciais.\n");
    }
    pausar();
}

float chaveOrdenacao(const Sessao *s, int criterio) {
    switch (criterio) {
    case CRIT_ID:
        return (float)s->id;
    case CRIT_ENERGIA:
        return s->kwhConsumido;
    case CRIT_CUSTO:
        return s->valorTotal;
    case CRIT_TEMPO:
        return (float)s->tempoConectado;
    default:
        return (float)s->id;
    }
}

const char *nomeCriterio(int criterio) {
    switch (criterio) {
    case CRIT_ID:
        return "ID";
    case CRIT_ENERGIA:
        return "energia consumida (kWh)";
    case CRIT_CUSTO:
        return "custo da sessao (R$)";
    case CRIT_TEMPO:
        return "tempo de recarga (min)";
    default:
        return "ID";
    }
}

void trocarSessoes(Sessao *a, Sessao *b) {
    Sessao temp = *a;
    *a = *b;
    *b = temp;
}

void bubbleSortHistorico(int criterio) {
    for (int i = 0; i < totalHistorico - 1; i++) {
        int trocou = 0;
        for (int j = 0; j < totalHistorico - 1 - i; j++) {
            if (chaveOrdenacao(&historico[j], criterio) >
                chaveOrdenacao(&historico[j + 1], criterio)) {
                trocarSessoes(&historico[j], &historico[j + 1]);
                trocou = 1;
            }
        }
        if (!trocou)
            break;
    }
}

void bubbleSortHistoricoDesc(int criterio) {
    for (int i = 0; i < totalHistorico - 1; i++) {
        int trocou = 0;
        for (int j = 0; j < totalHistorico - 1 - i; j++) {
            if (chaveOrdenacao(&historico[j], criterio) <
                chaveOrdenacao(&historico[j + 1], criterio)) {
                trocarSessoes(&historico[j], &historico[j + 1]);
                trocou = 1;
            }
        }
        if (!trocou)
            break;
    }
}

void ordenarSessoes(void) {
    sincronizarHistorico();
    cabecalho("ORDENAR SESSOES");

    if (totalHistorico < 2) {
        printf("  Sao necessarias pelo menos duas sessoes registradas para ordenar.\n");
        printf("  Sessoes registradas ate agora: %d\n", totalHistorico);
        pausar();
        return;
    }

    avisarPendentes();
    if (contarPendentes() > 0)
        printf("  [i] Ordenar por energia/custo agora pode nao refletir o resultado final.\n\n");

    printf("  Ordenar por:\n");
    printf("    [1] ID\n");
    printf("    [2] Energia consumida\n");
    printf("    [3] Custo da sessao\n");
    printf("    [4] Tempo de recarga\n");
    printf("    [0] Voltar\n");

    int criterio;
    if (!lerInteiro("  Escolha: ", 0, 4, &criterio) || criterio == 0)
        return;

    int ordem;
    printf("\n    [1] Crescente\n    [2] Decrescente\n");
    if (!lerInteiro("  Escolha: ", 1, 2, &ordem))
        return;

    if (ordem == 1)
        bubbleSortHistorico(criterio);
    else
        bubbleSortHistoricoDesc(criterio);

    printf("\n  Sessoes ordenadas por %s (%s) usando Bubble Sort.\n\n",
           nomeCriterio(criterio), ordem == 1 ? "crescente" : "decrescente");
    imprimirCabecalhoHistorico();
    for (int i = 0; i < totalHistorico; i++)
        imprimirLinhaHistorico(&historico[i]);
    sepTabela();
    printf("  %d sessao(oes) ordenada(s).\n", totalHistorico);
    pausar();
}

void mostrarEstatisticas(void) {
    sincronizarHistorico();
    cabecalho("ESTATISTICAS DA ESTACAO");

    if (totalHistorico == 0) {
        printf("  Nenhuma sessao registrada ainda.\n");
        printf("  Abra uma sessao na opcao [1] e simule/encerre para gerar dados.\n");
        pausar();
        return;
    }

    int concluidas = contarConcluidas();
    int pendentes = contarPendentes();

    float energiaTotal = 0.0f, energiaConcluidas = 0.0f;
    float faturamento = 0.0f;
    float maiorConsumo = 0.0f, menorConsumo = 0.0f;
    int tempoConcluidas = 0;
    int primeiro = 1;
    int idMaior = 0, idMenor = 0;

    for (int i = 0; i < totalHistorico; i++) {
        energiaTotal += historico[i].kwhConsumido;
        if (historico[i].status == SESS_CONCLUIDA) {
            energiaConcluidas += historico[i].kwhConsumido;
            faturamento += historico[i].valorTotal;
            tempoConcluidas += historico[i].tempoConectado;
            if (primeiro) {
                maiorConsumo = menorConsumo = historico[i].kwhConsumido;
                idMaior = idMenor = historico[i].id;
                primeiro = 0;
            } else {
                if (historico[i].kwhConsumido > maiorConsumo) {
                    maiorConsumo = historico[i].kwhConsumido;
                    idMaior = historico[i].id;
                }
                if (historico[i].kwhConsumido < menorConsumo) {
                    menorConsumo = historico[i].kwhConsumido;
                    idMenor = historico[i].id;
                }
            }
        }
    }

    avisarPendentes();

    printf("  [VOLUME]\n");
    sep();
    printf("  Sessoes registradas : %d\n", totalHistorico);
    printf("  Concluidas          : %d\n", concluidas);
    printf("  Em andamento        : %d\n", pendentes);
    printf("  Na fila de espera   : %d\n\n", tamFila);

    printf("  [ENERGIA]\n");
    sep();
    printf("  Energia total (todas as sessoes) : %.2f kWh\n", energiaTotal);
    printf("  Energia das sessoes concluidas   : %.2f kWh\n\n", energiaConcluidas);

    if (concluidas == 0) {
        printf("  [FINANCEIRO]\n");
        sep();
        printf("  Nenhuma sessao concluida ainda, portanto nao ha faturamento,\n");
        printf("  ticket medio nem consumo maximo/minimo para calcular.\n\n");
        printf("  Como gerar esses numeros:\n");
        printf("    1. Opcao [7] - simular a sessao existente, ou\n");
        printf("    2. Opcao [12] - encerrar a sessao ativa.\n");
        pausar();
        return;
    }

    float ticketMedio = faturamento / concluidas;
    float energiaMedia = energiaConcluidas / concluidas;
    float tempoMedio = (float)tempoConcluidas / concluidas;

    printf("  [FINANCEIRO  (base: %d sessao(oes) concluida(s))]\n", concluidas);
    sep();
    printf("  Faturamento total   : R$ %.2f\n", faturamento);
    printf("  Ticket medio        : R$ %.2f por sessao\n", ticketMedio);
    printf("  Energia media       : %.2f kWh por sessao\n", energiaMedia);
    printf("  Tempo medio         : %.1f min por sessao\n", tempoMedio);
    if (energiaConcluidas > 0.0f)
        printf("  Receita por kWh     : R$ %.2f\n", faturamento / energiaConcluidas);
    printf("\n");

    printf("  [EXTREMOS DE CONSUMO]\n");
    sep();
    printf("  Maior consumo       : %.2f kWh  (sessao #%d)\n", maiorConsumo, idMaior);
    printf("  Menor consumo       : %.2f kWh  (sessao #%d)\n", menorConsumo, idMenor);
    pausar();
}

void ocppEnviar(const char *tipo, const char *payload) {
    printf("  +- [%s] TX  %-30s +\n", OCPP_VERSAO, tipo);
    printf("  |  %s\n", payload);
    printf("  +--------------------------------------------------+\n");
}

void ocppReceber(const char *tipo, const char *resp) {
    printf("  +- [%s] RX  %-30s +\n", OCPP_VERSAO, tipo);
    printf("  |  %s\n", resp);
    printf("  +--------------------------------------------------+\n\n");
}

void modbusLog(const char *reg, float val, const char *un) {
    printf("  [MODBUS] %-22s = %8.2f %s\n", reg, val, un);
}

void simularConexaoOCPP(Sessao *s) {
    char buf[160];
    spinner("Conectando ao servidor OCPP", 18);
    sprintf(buf, "{\"connectorId\":%d,\"idTag\":\"%s\",\"meterStart\":0}", s->id, s->usuario);
    ocppEnviar("StartTransaction.req", buf);
    sprintf(buf, "{\"transactionId\":\"%s\",\"status\":\"Accepted\"}", s->ocppTxId);
    ocppReceber("StartTransaction.conf", buf);
    ocppEnviar("Heartbeat.req", "{}");
    ocppReceber("Heartbeat.conf", "{\"currentTime\":\"2025-06-11T10:00:00Z\"}");
    modbusLog("PowerActive_W", s->potenciaAtual * 1000, "W");
    modbusLog("BatterySOC_%", s->bateriaAtual, "%");
    modbusLog("GridDemand_kW", demandaTotal(), "kW");
}

void simularEncerramentoOCPP(Sessao *s) {
    char buf[160];
    spinner("Encerrando no servidor OCPP", 18);
    sprintf(buf, "{\"transactionId\":\"%s\",\"meterStop\":%.0f,\"reason\":\"Local\"}",
            s->ocppTxId, s->kwhConsumido * 1000);
    ocppEnviar("StopTransaction.req", buf);
    ocppReceber("StopTransaction.conf", "{\"status\":\"Accepted\"}");
    spinner("Sincronizando GoodWe Cloud", 14);
    printf("  [GoodWe] Sessao %s registrada no dashboard.\n\n", s->ocppTxId);
}

void fecharConta(Sessao *s) {
    if (s->tempoConectado > s->tempoEstimado)
        s->minutosExtras = s->tempoConectado - s->tempoEstimado;
    else
        s->minutosExtras = 0;

    float multa = s->minutosExtras * MULTA_OCIOSIDADE;
    s->valorTotal = s->kwhConsumido * s->tarifaKwh + TAXA_FIXA + multa;
    s->status = SESS_CONCLUIDA;
    atualizarHistorico(s);

    receitaTotal += s->valorTotal;
    kwhTotal += s->kwhConsumido;
    sessoesFinalizadas++;
    tempoTotalMin += s->tempoConectado;
}

void detalharCobranca(const Sessao *s) {
    float multa = s->minutosExtras * MULTA_OCIOSIDADE;
    sep();
    printf("  FECHAMENTO DA SESSAO #%d - %s\n", s->id, s->usuario);
    sep();
    printf("  Energia consumida : %.3f kWh x R$ %.2f = R$ %.2f\n",
           s->kwhConsumido, s->tarifaKwh, s->kwhConsumido * s->tarifaKwh);
    printf("  Taxa fixa         : R$ %.2f\n", TAXA_FIXA);
    if (multa > 0.0f)
        printf("  Multa ociosidade  : R$ %.2f (%d min alem do estimado)\n",
               multa, s->minutosExtras);
    printf("  TOTAL             : R$ %.2f\n", s->valorTotal);
    sep();
}

void abrirSessao(void) {
    cabecalho("NOVA SESSAO DE RECARGA");

    int slotLivre = -1;
    for (int i = 0; i < MAX_SESSOES; i++)
        if (sessoes[i].status == SESS_INATIVA || sessoes[i].status == SESS_CONCLUIDA) {
            slotLivre = i;
            break;
        }
    if (slotLivre < 0) {
        printf("  [X] Nenhum slot disponivel. Todos os %d conectores ocupados.\n", MAX_SESSOES);
        printf("      Encerre uma sessao na opcao [12] para liberar um conector.\n");
        pausar();
        return;
    }

    char nome[MAX_NOME];
    TipoCarregador tipo;
    float batInicial;
    int horaMin, tempoConec, t;

    if (!lerNome("  Nome do usuario: ", nome, MAX_NOME))
        return;

    printf("\n  Tipo de carregador:\n");
    printf("    [1] AC Lento        7 kW   R$ 3.50/kWh base\n");
    printf("    [2] AC Semirrapido  22 kW   R$ 4.20/kWh base\n");
    printf("    [3] DC Rapido       50 kW   R$ 5.20/kWh base\n");
    if (!lerInteiro("  Escolha: ", 1, 3, &t))
        return;
    tipo = (TipoCarregador)t;

    if (!lerFloat("  Bateria inicial (% 0-99): ", 0.0f, 99.0f, &batInicial))
        return;

    int tempoEstimadoPreview = estimarTempo(batInicial, potenciaBase(tipo));
    int hEst = tempoEstimadoPreview / 60;
    int mEst = tempoEstimadoPreview % 60;
    printf("\n  +------------------------------------------------+\n");
    printf("  |  PREVISAO DE CARGA COMPLETA (100%%)             |\n");
    printf("  |  Carregador  : %-30s  |\n", nomeTipo(tipo));
    printf("  |  Bateria     : %.1f%% -> 100%%                    |\n", batInicial);
    if (hEst > 0)
        printf("  |  Tempo est.  : %dh %02dmin (%d min total)          |\n",
               hEst, mEst, tempoEstimadoPreview);
    else
        printf("  |  Tempo est.  : %d min                           |\n", tempoEstimadoPreview);
    printf("  |  (Tempo acima disso gera multa de R$ %.2f/min) |\n", MULTA_OCIOSIDADE);
    printf("  +------------------------------------------------+\n\n");

    if (!lerHorario("  Hora de inicio (HH ou HH:MM): ", &horaMin))
        return;

    char prompt[96];
    sprintf(prompt, "  Tempo de conexao (minutos) [recomendado: %d]: ", tempoEstimadoPreview);
    if (!lerInteiro(prompt, 1, 1440, &tempoConec))
        return;

    if (tempoConec > tempoEstimadoPreview) {
        int extras = tempoConec - tempoEstimadoPreview;
        printf("\n  [!] AVISO: tempo informado excede o estimado em %d min.\n", extras);
        printf("  Multa de ociosidade estimada: R$ %.2f\n", extras * MULTA_OCIOSIDADE);
        if (!confirmar("  Deseja continuar assim mesmo? (1=Sim / 0=Nao): ")) {
            printf("  Operacao cancelada.\n");
            pausar();
            return;
        }
    }

    float dem = demandaTotal();
    if (dem / MAX_DEMANDA_KW >= LIMITE_FILA) {
        adicionarNaFila(nome, tipo, batInicial, horaMin, tempoConec);
        exibirFila();
        pausar();
        return;
    }

    Sessao *s = &sessoes[slotLivre];
    memset(s, 0, sizeof(Sessao));
    s->id = proximoId++;
    strncpy(s->usuario, nome, MAX_NOME - 1);
    s->usuario[MAX_NOME - 1] = '\0';
    s->tipo = tipo;
    s->bateriaInicial = batInicial;
    s->bateriaAtual = batInicial;
    s->horaInicio = horaMin;
    s->tempoConectado = tempoConec;
    s->potenciaAtual = potenciaBase(tipo);
    s->tarifaKwh = calcTarifa(tipo, horaMin);
    s->tempoEstimado = estimarTempo(batInicial, s->potenciaAtual);
    gerarTxId(s->ocppTxId, s->id);
    s->status = SESS_ATIVA;
    totalSessoes++;
    registrarHistorico(s);

    sep();
    simularConexaoOCPP(s);
    aplicarControleDemanda(0);

    printf("  [v] Sessao #%d aberta!\n", s->id);
    printf("  Transacao : %s\n", s->ocppTxId);
    printf("  Potencia  : %.1f kW\n", s->potenciaAtual);
    printf("  Tarifa    : R$ %.2f/kWh\n", s->tarifaKwh);
    printf("  Est. tempo: %d min\n\n", s->tempoEstimado);
    printf("  [i] A sessao esta ABERTA: energia e custo ainda estao em zero.\n");
    printf("      Use a opcao [7] para simular a recarga ou a [12] para encerrar,\n");
    printf("      e so entao os valores aparecem na listagem e nas estatisticas.\n");

    promoverDaFila();
    pausar();
}

void simularSessao(int idx) {
    Sessao *s = &sessoes[idx];
    if (s->status != SESS_ATIVA && s->status != SESS_THROTTLE) {
        printf("  [!] Sessao #%d nao esta ativa (status: %s).\n", s->id, nomeStatus(s->status));
        pausar();
        return;
    }

    cabecalho("SIMULANDO SESSAO");
    printf("  Usuario : %s\n", s->usuario);
    printf("  Tipo    : %s\n", nomeTipo(s->tipo));
    printf("  Potencia: %.1f kW\n", s->potenciaAtual);
    printf("  Tempo   : %d min\n\n", s->tempoConectado);

    float taxaG = (s->potenciaAtual / CAP_BATERIA_KWH * 100.0f) / 60.0f;

    for (int t = 1; t <= s->tempoConectado; t++) {
        if (s->bateriaAtual < 100.0f) {
            int lenta = (s->bateriaAtual >= 80.0f);
            float taxa = lenta ? taxaG * 0.45f : taxaG;
            float kwh = s->potenciaAtual * (lenta ? 0.45f : 1.0f) / 60.0f;
            s->bateriaAtual += taxa;
            if (s->bateriaAtual > 100.0f)
                s->bateriaAtual = 100.0f;
            s->kwhConsumido += kwh;
        }
        if (t % 10 == 0 || t == 1 || t == s->tempoConectado) {
            barra("Bateria", s->bateriaAtual, 25);
            printf("  Min %-4d  %.1f%%  %.3f kWh\n\n", t, s->bateriaAtual, s->kwhConsumido);
        }
    }

    fecharConta(s);
    simularEncerramentoOCPP(s);

    printf("  [v] Concluido!\n");
    printf("  Bateria final : %.1f%%\n", s->bateriaAtual);
    detalharCobranca(s);

    aplicarControleDemanda(0);
    promoverDaFila();
    if (tamFila > 0)
        exibirFila();

    pausar();
}

void painelSessoes(void) {
    cabecalho("PAINEL DE SESSOES");

    printf("  %-4s %-20s %-23s %-11s %-7s %-9s %-8s\n",
           "ID", "Usuario", "Tipo", "Status", "Bat%", "kWh", "kW");
    sepTabela();

    int alguma = 0;
    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status == SESS_INATIVA)
            continue;
        alguma = 1;
        printf("  #%-3d %-20s %-23s %-11s %5.1f%%  %7.3f  %6.1f\n",
               sessoes[i].id, sessoes[i].usuario, nomeTipo(sessoes[i].tipo),
               nomeStatus(sessoes[i].status), sessoes[i].bateriaAtual,
               sessoes[i].kwhConsumido, sessoes[i].potenciaAtual);
    }
    if (!alguma)
        printf("  Nenhuma sessao nos conectores.\n");

    sepTabela();
    float dem = demandaTotal();
    barra("Grid", dem / MAX_DEMANDA_KW * 100.0f, 25);
    printf("  Demanda: %.1f kW / %.0f kW\n\n", dem, MAX_DEMANDA_KW);

    exibirFila();
    pausar();
}

void dashboard(void) {
    limparTela();
    sincronizarHistorico();

    int ativas = 0;
    float demAtual = 0.0f, kwhSessaoAtiva = 0.0f;

    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE) {
            ativas++;
            demAtual += sessoes[i].potenciaAtual;
            kwhSessaoAtiva += sessoes[i].kwhConsumido;
        }
    }

    float utilizacao = demAtual / MAX_DEMANDA_KW * 100.0f;
    float ticketMedio = (sessoesFinalizadas > 0) ? receitaTotal / sessoesFinalizadas : 0.0f;
    float energiaMedia = (sessoesFinalizadas > 0) ? kwhTotal / sessoesFinalizadas : 0.0f;
    float tempoMedio = (sessoesFinalizadas > 0) ? (float)tempoTotalMin / sessoesFinalizadas : 0.0f;
    const char *ocppStatus = (ativas > 0 || tamFila > 0) ? "ONLINE" : "IDLE  ";

    printf("\n");
    sep2();
    printf("  |          DASHBOARD EXECUTIVO                 |\n");
    printf("  |          ChargeGrid Intelligence             |\n");
    sep2();
    printf("\n");

    printf("  [OPERACIONAL]\n");
    sep();
    printf("  Sessoes Ativas    : %d\n", ativas);
    printf("  Na Fila           : %d\n", tamFila);
    printf("  Total Registrado  : %d sessoes\n", totalHistorico);
    printf("\n");
    barra("Utilizacao Grid", utilizacao, 25);
    barra("Sessoes Ativas", (float)ativas / MAX_SESSOES * 100.0f, 25);
    printf("\n");
    printf("  Demanda Atual     : %.1f kW / %.0f kW\n", demAtual, MAX_DEMANDA_KW);
    printf("  Energia em curso  : %.3f kWh\n", kwhSessaoAtiva);
    printf("  OCPP              : [%s]\n", ocppStatus);

    sep();
    printf("  [FINANCEIRO  (sessoes concluidas)]\n");
    sep();
    printf("  Sessoes Concluidas: %d\n", sessoesFinalizadas);
    printf("  Energia Vendida   : %.3f kWh\n", kwhTotal);
    printf("  Receita Total     : R$ %8.2f\n", receitaTotal);
    if (sessoesFinalizadas == 0)
        printf("  (execute ou encerre sessoes para gerar faturamento)\n");

    sep();
    printf("  [KPIs DE NEGOCIO]\n");
    sep();
    if (sessoesFinalizadas > 0) {
        printf("  Ticket Medio      : R$ %7.2f / sessao\n", ticketMedio);
        printf("  Energia Media     : %.3f kWh / sessao\n", energiaMedia);
        printf("  Tempo Medio       : %.1f min / sessao\n", tempoMedio);
        if (kwhTotal > 0.0f)
            printf("  Receita por kWh   : R$ %7.2f\n", receitaTotal / kwhTotal);
    } else {
        printf("  (Nenhuma sessao finalizada ainda)\n");
    }

    sep();
    printf("  [CONECTORES]\n");
    sep();
    int cnt[4] = {0, 0, 0, 0};
    float pot[4] = {0, 0, 0, 0};
    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE) {
            int ti = (int)sessoes[i].tipo;
            if (ti >= 1 && ti <= 3) {
                cnt[ti]++;
                pot[ti] += sessoes[i].potenciaAtual;
            }
        }
    }
    printf("  AC Lento       ( 7 kW) : %d ativo(s)  %.1f kW\n", cnt[1], pot[1]);
    printf("  AC Semirrapido (22 kW) : %d ativo(s)  %.1f kW\n", cnt[2], pot[2]);
    printf("  DC Rapido      (50 kW) : %d ativo(s)  %.1f kW\n", cnt[3], pot[3]);

    sep2();
    pausar();
}

void relatorioCompleto(void) {
    cabecalho("RELATORIO GERAL DO SISTEMA");

    float totKwh = 0, totRec = 0;
    int conc = 0, atv = 0, alguma = 0;

    for (int i = 0; i < MAX_SESSOES; i++) {
        Sessao *s = &sessoes[i];
        if (s->status == SESS_INATIVA)
            continue;
        alguma = 1;

        printf("  +--- Sessao #%d ---------------------------------+\n", s->id);
        printf("  | Usuario       : %-28s |\n", s->usuario);
        printf("  | Tipo          : %-28s |\n", nomeTipo(s->tipo));
        printf("  | Status        : %-28s |\n", nomeStatus(s->status));
        printf("  | Bateria       : %.1f%% -> %.1f%%\n", s->bateriaInicial, s->bateriaAtual);
        printf("  | Energia       : %.3f kWh\n", s->kwhConsumido);
        printf("  | Potencia real : %.1f kW\n", s->potenciaAtual);
        printf("  | Tarifa kWh    : R$ %.2f\n", s->tarifaKwh);
        printf("  | Transacao     : %s\n", s->ocppTxId);

        if (s->status == SESS_CONCLUIDA) {
            float multa = s->minutosExtras * MULTA_OCIOSIDADE;
            printf("  | --- Cobranca ---\n");
            printf("  | Energia       : R$ %.2f\n", s->kwhConsumido * s->tarifaKwh);
            printf("  | Taxa fixa     : R$ %.2f\n", TAXA_FIXA);
            if (multa > 0.0f)
                printf("  | Multa ociosid.: R$ %.2f (%d min)\n", multa, s->minutosExtras);
            printf("  | TOTAL         : R$ %.2f\n", s->valorTotal);
            totKwh += s->kwhConsumido;
            totRec += s->valorTotal;
            conc++;
        } else {
            printf("  | --- Em andamento: valores ainda nao fechados ---\n");
            atv++;
        }
        printf("  +------------------------------------------------+\n\n");
    }

    if (!alguma)
        printf("  Nenhuma sessao nos conectores no momento.\n\n");

    sep();
    printf("  CONSOLIDADO (conectores)\n");
    printf("  Sessoes ativas    : %d\n", atv);
    printf("  Sessoes concluidas: %d\n", conc);
    printf("  Energia total     : %.3f kWh\n", totKwh);
    printf("  Receita total     : R$ %.2f\n", totRec);
    printf("  Demanda atual     : %.1f kW\n", demandaTotal());
    sep();
    printf("  Historico acumulado: %d sessoes | %.3f kWh | R$ %.2f\n",
           totalHistorico, kwhTotal, receitaTotal);
    sep();
    exibirFila();
    pausar();
}

void encerrarSessao(void) {
    cabecalho("ENCERRAR SESSAO");

    int alguma = 0;
    printf("  Sessoes em andamento:\n\n");
    printf("  %-5s %-20s %-23s %-11s %10s\n", "ID", "Usuario", "Tipo", "Status", "Energia");
    sepTabela();
    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE) {
            printf("  #%-4d %-20s %-23s %-11s %6.3f kWh\n",
                   sessoes[i].id, sessoes[i].usuario, nomeTipo(sessoes[i].tipo),
                   nomeStatus(sessoes[i].status), sessoes[i].kwhConsumido);
            alguma = 1;
        }
    }
    sepTabela();
    if (!alguma) {
        printf("  Nenhuma sessao ativa para encerrar.\n");
        pausar();
        return;
    }

    int escolha;
    if (!lerInteiro("\n  ID da sessao a encerrar (0=cancelar): ", 0, 999999, &escolha))
        return;
    if (escolha == 0) {
        printf("  Operacao cancelada.\n");
        pausar();
        return;
    }

    Sessao *s = NULL;
    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].id == escolha &&
            (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE)) {
            s = &sessoes[i];
            break;
        }
    }
    if (!s) {
        printf("  [!] Nenhuma sessao ATIVA com ID %d. Confira a lista acima.\n", escolha);
        pausar();
        return;
    }

    if (s->kwhConsumido <= 0.0f) {
        printf("\n  [!] A sessao #%d ainda nao foi simulada, entao o consumo e 0.000 kWh.\n", s->id);
        printf("      Encerrando agora, sera cobrada apenas a taxa fixa de R$ %.2f.\n", TAXA_FIXA);
        printf("      Dica: use a opcao [7] para simular a recarga antes de encerrar.\n");
        if (!confirmar("  Encerrar mesmo assim? (1=Sim / 0=Nao): ")) {
            printf("  Operacao cancelada.\n");
            pausar();
            return;
        }
    }

    fecharConta(s);
    simularEncerramentoOCPP(s);
    printf("  [v] Sessao #%d encerrada.\n", s->id);
    detalharCobranca(s);
    printf("  Conector liberado. Os valores ja aparecem na listagem,\n");
    printf("  nas estatisticas e na ordenacao.\n");

    aplicarControleDemanda(0);
    promoverDaFila();
    if (tamFila > 0)
        exibirFila();

    pausar();
}

void cenarioDemo(void) {
    cabecalho("CENARIO DE DEMONSTRACAO AUTOMATICA");

    int emCarga = contarSessoesEmCarga();
    if (emCarga > 0 || tamFila > 0) {
        printf("  [!] Existem %d sessao(oes) em carga e %d na fila.\n", emCarga, tamFila);
        printf("      O cenario de demonstracao reinicia os conectores e descarta\n");
        printf("      essas sessoes (elas serao removidas do historico).\n");
        if (!confirmar("  Continuar? (1=Sim / 0=Nao): ")) {
            printf("  Operacao cancelada.\n");
            pausar();
            return;
        }
        for (int i = 0; i < MAX_SESSOES; i++)
            if (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE)
                removerDoHistorico(sessoes[i].id);
    }

    printf("\n  Simulando 4 veiculos (1 pode ir para a fila)...\n\n");
    spinner("Preparando cenario", 25);

    memset(sessoes, 0, sizeof(sessoes));
    for (int i = 0; i < MAX_SESSOES; i++)
        sessoes[i].status = SESS_INATIVA;
    tamFila = 0;

    struct {
        const char *nome;
        TipoCarregador tipo;
        float bat;
        int hini;
        int tcon;
    } veics[] = {
        {"Ana Costa", TIPO_AC_LENTO, 20.0f, 480, 60},
        {"Bruno Lima", TIPO_AC_RAPIDO, 40.0f, 1080, 45},
        {"Carla Souza", TIPO_DC_ULTRA, 5.0f, 180, 30},
        {"Diego Rocha", TIPO_AC_RAPIDO, 60.0f, 900, 40},
    };

    for (int v = 0; v < 4; v++) {
        float dem = demandaTotal();
        int slotLivre = -1;
        for (int i = 0; i < MAX_SESSOES; i++)
            if (sessoes[i].status == SESS_INATIVA || sessoes[i].status == SESS_CONCLUIDA) {
                slotLivre = i;
                break;
            }

        if (slotLivre >= 0 && dem / MAX_DEMANDA_KW < LIMITE_FILA) {
            Sessao *s = &sessoes[slotLivre];
            memset(s, 0, sizeof(Sessao));
            s->id = proximoId++;
            strncpy(s->usuario, veics[v].nome, MAX_NOME - 1);
            s->usuario[MAX_NOME - 1] = '\0';
            s->tipo = veics[v].tipo;
            s->bateriaInicial = veics[v].bat;
            s->bateriaAtual = veics[v].bat;
            s->horaInicio = veics[v].hini;
            s->tempoConectado = veics[v].tcon;
            s->potenciaAtual = potenciaBase(veics[v].tipo);
            s->tarifaKwh = calcTarifa(veics[v].tipo, veics[v].hini);
            s->tempoEstimado = estimarTempo(veics[v].bat, s->potenciaAtual);
            gerarTxId(s->ocppTxId, s->id);
            s->status = SESS_ATIVA;
            totalSessoes++;
            registrarHistorico(s);
            printf("  [+] Sessao #%d: %s  (%s)\n", s->id, s->usuario, nomeTipo(s->tipo));
        } else {
            adicionarNaFila(veics[v].nome, veics[v].tipo, veics[v].bat,
                            veics[v].hini, veics[v].tcon);
        }
    }

    printf("\n");
    sep();
    float dem = demandaTotal();
    printf("  Demanda apos abertura: %.1f kW (%.0f%%)\n\n", dem, dem / MAX_DEMANDA_KW * 100.0f);
    barra("Utilizacao", dem / MAX_DEMANDA_KW * 100.0f, 25);
    printf("\n");
    exibirFila();
    sep();

    aplicarControleDemanda(1);

    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status != SESS_ATIVA && sessoes[i].status != SESS_THROTTLE)
            continue;
        Sessao *s = &sessoes[i];

        printf("\n  * Simulando sessao #%d  %s\n", s->id, s->usuario);
        spinner("Processando", 16);

        float taxaG = (s->potenciaAtual / CAP_BATERIA_KWH * 100.0f) / 60.0f;
        for (int t = 1; t <= s->tempoConectado; t++) {
            if (s->bateriaAtual < 100.0f) {
                int lenta = (s->bateriaAtual >= 80.0f);
                s->bateriaAtual += lenta ? taxaG * 0.45f : taxaG;
                if (s->bateriaAtual > 100.0f)
                    s->bateriaAtual = 100.0f;
                s->kwhConsumido += s->potenciaAtual * (lenta ? 0.45f : 1.0f) / 60.0f;
            }
        }

        fecharConta(s);
        barra("Bateria final", s->bateriaAtual, 25);
        printf("  Energia: %.3f kWh  |  R$ %.2f\n", s->kwhConsumido, s->valorTotal);

        aplicarControleDemanda(0);
        promoverDaFila();
    }

    printf("\n");
    sep();
    printf("  RESUMO DO CENARIO\n");
    sep();
    float recT = 0, kwhT = 0;
    for (int i = 0; i < MAX_SESSOES; i++) {
        if (sessoes[i].status == SESS_CONCLUIDA) {
            recT += sessoes[i].valorTotal;
            kwhT += sessoes[i].kwhConsumido;
            printf("  %-15s  %5.1f%%  %7.3f kWh  R$ %7.2f\n",
                   sessoes[i].usuario, sessoes[i].bateriaAtual,
                   sessoes[i].kwhConsumido, sessoes[i].valorTotal);
        }
    }
    sep();
    printf("  Energia total  : %.3f kWh\n", kwhT);
    printf("  Receita total  : R$ %.2f\n", recT);
    printf("\n  [i] Os dados ja estao no historico: veja as opcoes [2], [4] e [5].\n");
    if (tamFila > 0) {
        printf("\n");
        exibirFila();
    }

    pausar();
}

void menuPrincipal(void) {
    int opc = -1;
    do {
        limparTela();
        printf("\n");
        printf("  +================================================+\n");
        printf("  |                 ASTERCHARGE                    |\n");
        printf("  |     Sistema Inteligente de Recarga EV          |\n");
        printf("  |     GoodWe Ecosystem  |  FIAP                  |\n");
        printf("  +================================================+\n");

        float dem = demandaTotal();
        float ratio = dem / MAX_DEMANDA_KW * 100.0f;
        int filled = (int)(ratio / 100.0f * 16);
        if (filled > 16)
            filled = 16;
        printf("  |  Grid [");
        for (int i = 0; i < 16; i++)
            printf(i < filled ? "#" : ".");
        printf("] %5.1f%%  |\n", ratio);

        printf("  |  Ativas: %-2d  Fila: %-2d  Receita: R$ %7.2f  |\n",
               contarSessoesEmCarga(), tamFila, receitaTotal);
        printf("  +================================================+\n");
        printf("  |  ---- GESTAO DE SESSOES (Sprint 3) ----        |\n");
        printf("  |  [1] Nova sessao de recarga                    |\n");
        printf("  |  [2] Listar sessoes                            |\n");
        printf("  |  [3] Buscar sessao (linear / binaria)          |\n");
        printf("  |  [4] Ordenar sessoes (Bubble Sort)             |\n");
        printf("  |  [5] Estatisticas                              |\n");
        printf("  |  ---- OPERACAO DA ESTACAO (Sprint 2) ----      |\n");
        printf("  |  [6] Painel de sessoes ativas + fila           |\n");
        printf("  |  [7] Simular sessao existente                  |\n");
        printf("  |  [8] >> DASHBOARD EXECUTIVO <<                 |\n");
        printf("  |  [9] Relatorio completo                        |\n");
        printf("  |  [10] Cenario de demonstracao (4 veiculos)     |\n");
        printf("  |  [11] Controle de demanda                      |\n");
        printf("  |  [12] Encerrar sessao ativa                    |\n");
        printf("  |  [0] Sair                                      |\n");
        printf("  +================================================+\n");

        if (!lerInteiro("  Opcao: ", 0, 12, &opc)) {
            printf("\n  Entrada encerrada. Saindo...\n");
            return;
        }

        switch (opc) {
        case 1:
            limparTela();
            abrirSessao();
            break;
        case 2:
            limparTela();
            listarSessoes();
            break;
        case 3:
            limparTela();
            buscarSessao();
            break;
        case 4:
            limparTela();
            ordenarSessoes();
            break;
        case 5:
            limparTela();
            mostrarEstatisticas();
            break;
        case 6:
            limparTela();
            painelSessoes();
            break;
        case 7: {
            limparTela();
            cabecalho("SIMULAR SESSAO EXISTENTE");

            int disponivel = 0;
            printf("  Sessoes disponiveis para simulacao:\n\n");
            for (int i = 0; i < MAX_SESSOES; i++) {
                if (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE) {
                    printf("    #%-3d %-20s %s  (%d min previstos)\n",
                           sessoes[i].id, sessoes[i].usuario,
                           nomeTipo(sessoes[i].tipo), sessoes[i].tempoConectado);
                    disponivel = 1;
                }
            }
            if (!disponivel) {
                printf("    Nenhuma sessao ativa. Abra uma na opcao [1].\n");
                pausar();
                break;
            }

            int id;
            if (!lerInteiro("\n  ID da sessao para simular (0=cancelar): ", 0, 999999, &id))
                break;
            if (id == 0)
                break;

            int indice = -1;
            for (int i = 0; i < MAX_SESSOES; i++) {
                if (sessoes[i].id == id &&
                    (sessoes[i].status == SESS_ATIVA || sessoes[i].status == SESS_THROTTLE)) {
                    indice = i;
                    break;
                }
            }

            if (indice >= 0) {
                limparTela();
                simularSessao(indice);
            } else {
                printf("  [!] Nenhuma sessao ativa com ID %d.\n", id);
                pausar();
            }
            break;
        }
        case 8:
            dashboard();
            break;
        case 9:
            limparTela();
            relatorioCompleto();
            break;
        case 10:
            limparTela();
            cenarioDemo();
            break;
        case 11:
            limparTela();
            painelDemanda();
            break;
        case 12:
            limparTela();
            encerrarSessao();
            break;
        case 0:
            printf("\n  Ate logo! *\n\n");
            break;
        default:
            printf("  [!] Opcao invalida.\n");
            pausar();
        }
    } while (opc != 0);
}

int main(void) {
    memset(sessoes, 0, sizeof(sessoes));
    for (int i = 0; i < MAX_SESSOES; i++)
        sessoes[i].status = SESS_INATIVA;
    proximoId = 1;
    menuPrincipal();
    return 0;
}