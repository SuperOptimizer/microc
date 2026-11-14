# guest.sh: run inside the VM by tests/ulibs.sh, in the shared directory. TESTS names the programs; NOW is
# the host's time (the guest has no clock of its own, and certificates have dates).
date -s @$NOW >/dev/null 2>&1
echo "127.0.0.1 localhost" > /etc/hosts
mkdir -p /www/dir; cp data/plain.bin /www/; echo index > /www/dir/index.html
httpd -p 127.0.0.1:8080 -h /www
./tls_server 8081 - - /www
./tls_server 8443 data/tls-key.der data/tls-cert.der /www
for t in $TESTS; do
    ./$t
    echo "exit $t $?"
done
