FROM debian:bookworm-slim

# Instala dependências de sistema
# - build-essential: para compilar o código
# - portaudio19-dev: cabeçalhos do PortAudio
# - libportaudio2: biblioteca de execução
# - iproute2: comandos 'ip'
# - openssh-client: para testar a conexão saindo do container
# - openssh-server: caso queira que o container receba conexões
RUN apt-get update && apt-get install -y \
    build-essential \
    libportaudio2 \
    portaudio19-dev \
    liblz4-dev \
    iproute2 \
    iputils-ping \
    openssh-client \
    openssh-server \
    socat \
    && rm -rf /var/lib/apt/lists/*

# Configura o diretório de trabalho
WORKDIR /app

# Copia os arquivos do projeto
COPY . .

# Garante que o projeto seja compilado dentro do ambiente Linux do Docker
RUN make clean && make

# Configuração opcional para permitir SSH no root (senha: echo)
RUN mkdir /var/run/sshd && \
    echo 'root:echo' | chpasswd && \
    sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin yes/' /etc/ssh/sshd_config

# Expondo a porta do SSH caso necessário
EXPOSE 22

CMD ["/bin/bash"]
