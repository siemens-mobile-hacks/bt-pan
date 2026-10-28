#!/bin/bash
set -e
BR=pan0
NET=192.168.7
DNSMASQ_PID=/run/btpan-dnsmasq.pid
SPEED_PID=/run/btpan-speedtest.pid
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ "$1" = "stop" ]; then
	sudo pkill -f "bt-network -s nap" 2>/dev/null || true
	[ -f "$DNSMASQ_PID" ] && sudo kill "$(cat "$DNSMASQ_PID")" 2>/dev/null || true
	[ -f "$SPEED_PID" ] && sudo kill "$(cat "$SPEED_PID")" 2>/dev/null || true
	sudo iptables -t nat -D POSTROUTING -s $NET.0/24 -j MASQUERADE 2>/dev/null || true
	sudo iptables -D FORWARD -i $BR -j ACCEPT 2>/dev/null || true
	sudo iptables -D FORWARD -o $BR -j ACCEPT 2>/dev/null || true
	sudo iptables -t mangle -D FORWARD -o $BR -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu 2>/dev/null || true
	sudo iptables -t mangle -D FORWARD -i $BR -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu 2>/dev/null || true
	sudo ip link delete $BR 2>/dev/null || true
	echo "BT PAN host stopped"
	exit 0
fi

sudo modprobe bnep

sudo pkill -f "bt-network -s nap" 2>/dev/null || true
[ -f "$DNSMASQ_PID" ] && sudo kill "$(cat "$DNSMASQ_PID")" 2>/dev/null || true

sudo ip link delete $BR 2>/dev/null || true
sudo ip link add $BR type bridge
sudo ip addr add $NET.1/24 dev $BR
sudo ip link set $BR up
# MTU=320 ./nap.sh for S65 without bt-pan-mtu_S65v58.vkp (L2CAP MTU 335)
sudo ip link set $BR mtu ${MTU:-1500}

sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -t nat -C POSTROUTING -s $NET.0/24 -j MASQUERADE 2>/dev/null || \
	sudo iptables -t nat -A POSTROUTING -s $NET.0/24 -j MASQUERADE
sudo iptables -C FORWARD -i $BR -j ACCEPT 2>/dev/null || sudo iptables -A FORWARD -i $BR -j ACCEPT
sudo iptables -C FORWARD -o $BR -j ACCEPT 2>/dev/null || sudo iptables -A FORWARD -o $BR -j ACCEPT
# small MTU: TCP from the internet must not exceed it
if [ "${MTU:-1500}" -lt 1500 ]; then
	sudo iptables -t mangle -C FORWARD -o $BR -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu 2>/dev/null || \
		sudo iptables -t mangle -A FORWARD -o $BR -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu
	sudo iptables -t mangle -C FORWARD -i $BR -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu 2>/dev/null || \
		sudo iptables -t mangle -A FORWARD -i $BR -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu
fi

sudo dnsmasq \
	--interface=$BR --bind-interfaces --except-interface=lo \
	--dhcp-range=$NET.10,$NET.100,255.255.255.0,12h \
	--dhcp-option=3,$NET.1 \
	--dhcp-option=6,8.8.8.8,1.1.1.1 \
	--dhcp-authoritative \
	--pid-file=$DNSMASQ_PID

if command -v bt-network >/dev/null 2>&1; then
	sudo bt-network -s nap $BR &
	echo "NAP server (bt-network) started on $BR"
else
	echo "bluez-tools not found"
fi

# сервер спидтеста для эльфа
[ -f "$SPEED_PID" ] && sudo kill "$(cat "$SPEED_PID")" 2>/dev/null || true
if [ -f "$HERE/speedtest-pc.py" ]; then
	python3 "$HERE/speedtest-pc.py" $NET.1 >/tmp/btpan-speedtest.log 2>&1 &
	echo $! | sudo tee "$SPEED_PID" >/dev/null
	echo "speedtest server on $NET.1 (лог /tmp/btpan-speedtest.log)"
fi

echo "BT PAN host up: $BR=$NET.1/24, DHCP $NET.10-100, NAT on. Остановить: ./nap.sh stop"
