#!/usr/bin/env bash
# Roda a suite de testes como root: cobre os caminhos de TUN que exigem
# privilegios (tun_alloc, echo_init, tun_set_ip/mtu/up).
# Autorizado sem senha via /etc/sudoers.d/echo-protocol.
# ATENCAO: este script roda com privilegios de root. Nao o edite para
# executar outras coisas; altere os fontes/testes do projeto em vez disso.
set -e
cd "$(dirname "$0")"

if [ "$(id -u)" -ne 0 ]; then
    echo "Este script deve rodar como root (caminhos de TUN exigem privilegios)." >&2
    echo "Uso: sudo /home/vinisskt/Projects/echo-protocol/tests/root_tests.sh" >&2
    exit 1
fi

trap 'make clean >/dev/null 2>&1 || true' EXIT

make run_tests
make coverage