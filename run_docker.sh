#!/bin/bash

IMAGE_NAME="echo-protocol-vm"

echo "[Docker] Construindo imagem $IMAGE_NAME..."
docker build -t $IMAGE_NAME .

echo "[Docker] Iniciando container isolado..."
echo "[Dica] Use './echo-protocol -l' dentro do container para ver os dispositivos de áudio."

# --cap-add=NET_ADMIN: Permite criar interfaces TUN
# --device /dev/net/tun: Acesso ao driver de rede
# --device /dev/snd: Acesso ao áudio (Loopback) do Host
docker run -it --rm \
    --name echo_vm \
    --cap-add=NET_ADMIN \
    --device /dev/net/tun \
    --device /dev/snd \
    $IMAGE_NAME
