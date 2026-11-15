#!/bin/bash
# ПК-сторона BT PAN: мост pan0 + NAT + DHCP-сервер + NAP-сервер.
# Телефон (PANU) подключается к нашему NAP, BlueZ заводит bnepX и вводит его в мост pan0.
# Останов: ./nap.sh stop

set -e
BR=pan0
NET=192.168.7
DNSMASQ_PID=/run/btpan-dnsmasq.pid
SPEED_PID=/run/btpan-speedtest.pid
ROLE_PID=/run/btpan-role.pid
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ "$1" = "stop" ]; then
	sudo pkill -f "bt-network -s nap" 2>/dev/null || true
	[ -f "$DNSMASQ_PID" ] && sudo kill "$(cat "$DNSMASQ_PID")" 2>/dev/null || true
	[ -f "$SPEED_PID" ] && sudo kill "$(cat "$SPEED_PID")" 2>/dev/null || true
	[ -f "$ROLE_PID" ] && sudo kill "$(cat "$ROLE_PID")" 2>/dev/null || true
	sudo iptables -t nat -D POSTROUTING -s $NET.0/24 -j MASQUERADE 2>/dev/null || true
	sudo iptables -D FORWARD -i $BR -j ACCEPT 2>/dev/null || true
	sudo iptables -D FORWARD -o $BR -j ACCEPT 2>/dev/null || true
	sudo iptables -t mangle -D FORWARD -o $BR -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu 2>/dev/null || true
	sudo iptables -t mangle -D FORWARD -i $BR -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu 2>/dev/null || true
	sudo ip link delete $BR 2>/dev/null || true
	echo "BT PAN host stopped"
	exit 0
fi

# модуль Bluetooth-сети (без него ничего не заработает!)
sudo modprobe bnep

# убрать старые запуски, если были
sudo pkill -f "bt-network -s nap" 2>/dev/null || true
[ -f "$DNSMASQ_PID" ] && sudo kill "$(cat "$DNSMASQ_PID")" 2>/dev/null || true

# сетевой мост с IP-шлюзом для телефона
sudo ip link delete $BR 2>/dev/null || true
sudo ip link add $BR type bridge
sudo ip addr add $NET.1/24 dev $BR
sudo ip link set $BR up
# MTU=320 ./nap.sh for S65 (bt-pan_SG: stock L2CAP MTU 335, see src/config.h)
sudo ip link set $BR mtu ${MTU:-1500}

# раздача интернета (NAT)
sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -t nat -C POSTROUTING -s $NET.0/24 -j MASQUERADE 2>/dev/null || \
	sudo iptables -t nat -A POSTROUTING -s $NET.0/24 -j MASQUERADE
sudo iptables -C FORWARD -i $BR -j ACCEPT 2>/dev/null || sudo iptables -A FORWARD -i $BR -j ACCEPT
sudo iptables -C FORWARD -o $BR -j ACCEPT 2>/dev/null || sudo iptables -A FORWARD -o $BR -j ACCEPT
# small MTU: TCP from the internet must not exceed it (DF packets are not fragmented)
if [ "${MTU:-1500}" -lt 1500 ]; then
	sudo iptables -t mangle -C FORWARD -o $BR -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu 2>/dev/null || \
		sudo iptables -t mangle -A FORWARD -o $BR -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu
	sudo iptables -t mangle -C FORWARD -i $BR -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu 2>/dev/null || \
		sudo iptables -t mangle -A FORWARD -i $BR -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu
fi

# DHCP-сервер на мосту (под DHCP-клиент в эльфе). Отдельный pid-файл, системный dnsmasq не трогаем.
sudo dnsmasq \
	--interface=$BR --bind-interfaces --except-interface=lo \
	--dhcp-range=$NET.10,$NET.100,255.255.255.0,12h \
	--dhcp-option=3,$NET.1 \
	--dhcp-option=6,8.8.8.8,1.1.1.1 \
	--dhcp-authoritative \
	--pid-file=$DNSMASQ_PID

# NAP-сервер: принимает PANU-подключение телефона и вводит bnepX в мост pan0.
# bt-network из bluez-tools; если его нет — включите NAP через BlueZ Network1 вручную.
if command -v bt-network >/dev/null 2>&1; then
	sudo bt-network -s nap $BR &
	echo "NAP server (bt-network) started on $BR"
else
	echo "WARN: bt-network не найден (пакет bluez-tools). Включите NAP-сервер BlueZ вручную,"
	echo "      иначе телефон не сможет подключиться к PSM 0x000f."
fi

# сервер спидтеста для эльфа (UDP 5001 напрямую, TCP 5003 через стек телефона)
[ -f "$SPEED_PID" ] && sudo kill "$(cat "$SPEED_PID")" 2>/dev/null || true
if [ -f "$HERE/speedtest-pc.py" ]; then
	python3 "$HERE/speedtest-pc.py" $NET.1 >/tmp/btpan-speedtest.log 2>&1 &
	echo $! | sudo tee "$SPEED_PID" >/dev/null
	echo "speedtest server on $NET.1 (лог /tmp/btpan-speedtest.log)"
fi

# PC = BT master (central). The phone pages the PC, so the PC starts as slave and the phone
# (BT 1.2) polls it rarely: PC->phone downloads were poll-limited (16 KB/s as slave, 84 as master,
# EL71 2026-10-21). The switch is lost on every reconnect, so a watcher redoes it every 2 s.
# Only for the phone given in PHONE (other BT devices of the PC are not touched):
#   PHONE=00:0D:41:21:76:1D ./nap.sh
[ -f "$ROLE_PID" ] && sudo kill "$(cat "$ROLE_PID")" 2>/dev/null || true
if [ -n "$PHONE" ] && command -v hcitool >/dev/null 2>&1; then
	LE=$(echo "$PHONE" | awk -F: '{print $6,$5,$4,$3,$2,$1}')
	sudo bash -c "while :; do hcitool con | grep -i '$PHONE' | grep -Eq 'PERIPHERAL|SLAVE' && \
		hcitool cmd 0x02 0x0b $LE 00 >/dev/null 2>&1; sleep 2; done" >/dev/null 2>&1 &
	echo $! | sudo tee "$ROLE_PID" >/dev/null
	echo "BT role watcher: PC master for $PHONE (check: hcitool con -> CENTRAL/MASTER)"
fi

echo "BT PAN host up: $BR=$NET.1/24, DHCP $NET.10-100, NAT on. Остановить: ./nap.sh stop"
