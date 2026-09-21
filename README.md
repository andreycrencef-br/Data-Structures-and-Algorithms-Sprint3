# Data-Structures-and-Algorithms-Sprint3

## Integrantes:

Andrey Crence Fernandes - RM 573840

Giulliana Maistro Brasolin - RM 569381

Mikaella Mirela Dos Santos Lucindo - RM 573775

Lara Dos Santos Cândido Alves - RM 573827

Lucas Parkin Devito - RM 573251

## Sobre o Projeto

O AsterCharge é um sistema em linguagem C que simula o funcionamento de uma estação de recarga de veículos elétricos. O sistema simula uma lógica de cobrança (energia consumida, tarifas, taxas e multas), controle de demanda da rede elétrica, fila de espera para veículos quando a capacidade está no limite, e a comunicação com o carregador através de mensagens no estilo do protocolo OCPP, aproximando o projeto do funcionamento real de um eletroposto.

## Tecnologias Utilizadas
- Linguagem C (padrão C11)
- Estruturas de dados: struct e typedef
- Vetores de estruturas para armazenamento de múltiplas sessões
- Manipulação de strings da biblioteca string.h
- Entrada e saída de dados via stdio.h

## Funcionalidades
- Cadastro de novas sessões de recarga (nome, tipo de carregador, bateria inicial, horário e tempo de conexão)
- Cálculo automático de tarifa, tempo estimado de recarga e valor total da sessão
- Listagem de todas as sessões registradas
- Busca de sessões por ID (linear e binária) e por nome do usuário (linear)
- Ordenação das sessões por ID, energia consumida, custo ou tempo de recarga (Bubble Sort)
- Estatísticas da estação: total de sessões, energia fornecida, faturamento, ticket médio, maior e menor consumo
- Painel de sessões ativas e fila de espera em tempo real
- Simulação da recarga minuto a minuto, com evolução da bateria
- Controle de demanda: redução automática de potência quando a rede se aproxima do limite
- Dashboard executivo com indicadores operacionais e financeiros
- Relatório completo com o detalhamento de cada sessão
- Cenário de demonstração automática com múltiplos veículos
