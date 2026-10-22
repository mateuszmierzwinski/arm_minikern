// Command nettest exercises Go's net package on minikern: UDP as a server
// (reached from the host through QEMU's hostfwd), as a client (to a host
// listener behind slirp's 10.0.2.2), and over loopback. It prints PASS or
// FAIL per check and exits 0 only when all pass. The host side is driven
// by test/net_sockets_qemu_test.sh, which fixes the ports below.
package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strings"
	"syscall"
	"time"
)

const (
	udpServerPort = 9000 // forwarded from the host
	tcpServerPort = 9001 // the same, over TCP
	// Where the host's UDP echo listens, from the environment; slirp maps
	// 10.0.2.2 to the host for UDP, so the test passes an address there.
	udpEchoEnv = "MINIKERN_UDP_ECHO"
	// The gateway's discard port: a send to it needs no listener, so it
	// tells us whether DHCP has given us an address yet.
	gatewayDiscard = "10.0.2.2:9"
	// Where the TCP echo lives, passed in MINIKERN_TCP_ECHO by the test so
	// the address can be the host's own on the real network: slirp then
	// carries the segments out through the EMAC and back, which is the one
	// path a loopback connection does not exercise.
	tcpEchoEnv = "MINIKERN_TCP_ECHO"

	// The name the DNS test resolves, and the address it must come back
	// as, both passed in by the test: the server answering is one of ours
	// (test/tools/dnsecho), so this proves the resolver path rather than
	// the internet.
	dnsNameEnv   = "MINIKERN_DNS_NAME"
	dnsAnswerEnv = "MINIKERN_DNS_ANSWER"

	// Where the test's own resolver listens. It is not port 53, because
	// binding 53 on the build machine needs root and QEMU's guestfwd is
	// TCP-only — so the query is aimed there by a Dial of our own. What
	// that leaves untested is one line of /etc/resolv.conf, which the
	// check below reads directly instead.
	dnsServerEnv = "MINIKERN_DNS_SERVER"
	networkWait  = 10 * time.Second
)

var failed bool

// errSkip means a check needs something this run was not given.
var errSkip = errors.New("skipped: nothing to test against")

func check(name string, err error) {
	if err == nil {
		fmt.Println("PASS", name)
		return
	}
	if errors.Is(err, errSkip) {
		fmt.Println("SKIP", name)
		return
	}
	fmt.Println("FAIL", name+":", err)
	failed = true
}

// waitForNetwork retries until DHCP has given the kernel an address (a send
// before that is ENETUNREACH).
func waitForNetwork() error {
	deadline := time.Now().Add(networkWait)
	for time.Now().Before(deadline) {
		c, err := net.Dial("udp", gatewayDiscard)
		if err == nil {
			_, err = c.Write([]byte("probe"))
			c.Close()
		}
		if err == nil {
			return nil
		}
		if !errors.Is(err, syscall.ENETUNREACH) {
			return err
		}
		time.Sleep(100 * time.Millisecond)
	}
	return errors.New("no address after " + networkWait.String())
}

// udpServer answers one datagram from the host with its upper-case form.
func udpServer() error {
	c, err := net.ListenUDP("udp4", &net.UDPAddr{Port: udpServerPort})
	if err != nil {
		return err
	}
	defer c.Close()
	fmt.Println("READY udp-server", c.LocalAddr())
	buf := make([]byte, 1500)
	c.SetReadDeadline(time.Now().Add(20 * time.Second))
	n, from, err := c.ReadFromUDP(buf)
	if err != nil {
		return err
	}
	_, err = c.WriteToUDP(bytes.ToUpper(buf[:n]), from)
	return err
}

// udpClient sends to the host's echo listener and expects the same bytes.
func udpClient() error {
	target := os.Getenv(udpEchoEnv)
	if target == "" {
		return errSkip
	}
	c, err := net.Dial("udp", target)
	if err != nil {
		return err
	}
	defer c.Close()
	msg := []byte("hello host from minikern")
	buf := make([]byte, 64)
	c.SetReadDeadline(time.Now().Add(5 * time.Second))
	for tries := 0; tries < 5; tries++ {
		if _, err = c.Write(msg); err != nil {
			return err
		}
		n, err := c.Read(buf)
		if err == nil && bytes.Equal(buf[:n], msg) {
			return nil
		}
		if err != nil && !strings.Contains(err.Error(), "timeout") && !errors.Is(err, syscall.ECONNREFUSED) {
			return err
		}
	}
	return errors.New("no echo from " + target)
}

// loopback: two sockets on 127.0.0.1 talk to each other.
func loopback() error {
	a, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		return err
	}
	defer a.Close()
	b, err := net.DialUDP("udp4", nil, a.LocalAddr().(*net.UDPAddr))
	if err != nil {
		return err
	}
	defer b.Close()
	if _, err := b.Write([]byte("loop")); err != nil {
		return err
	}
	buf := make([]byte, 16)
	a.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, from, err := a.ReadFromUDP(buf)
	if err != nil {
		return err
	}
	if string(buf[:n]) != "loop" || from.Port != b.LocalAddr().(*net.UDPAddr).Port {
		return fmt.Errorf("got %q from %v", buf[:n], from)
	}
	return nil
}

// tcpClient dials the echo service across the wire and expects its bytes
// back: an active open, data both ways and a close, all over the EMAC.
func tcpClient() error {
	target := os.Getenv(tcpEchoEnv)
	if target == "" {
		return errSkip
	}
	c, err := net.DialTimeout("tcp", target, 10*time.Second)
	if err != nil {
		return err
	}
	defer c.Close()
	msg := []byte("tcp hello from minikern")
	if _, err := c.Write(msg); err != nil {
		return err
	}
	buf := make([]byte, len(msg))
	c.SetReadDeadline(time.Now().Add(5 * time.Second))
	if _, err := io.ReadFull(c, buf); err != nil {
		return err
	}
	if !bytes.Equal(buf, msg) {
		return fmt.Errorf("got %q", buf)
	}
	return nil
}

// tcpServer answers one connection from the host with the upper-cased
// line it sent, then closes.
func tcpServer() error {
	l, err := net.Listen("tcp4", fmt.Sprintf(":%d", tcpServerPort))
	if err != nil {
		return err
	}
	defer l.Close()
	fmt.Println("READY tcp-server", l.Addr())
	c, err := l.Accept()
	if err != nil {
		return err
	}
	defer c.Close()
	buf := make([]byte, 64)
	c.SetReadDeadline(time.Now().Add(20 * time.Second))
	n, err := c.Read(buf)
	if err != nil {
		return err
	}
	_, err = c.Write(bytes.ToUpper(buf[:n]))
	return err
}

// httpOverLoopback runs an HTTP server and a client against it inside the
// guest: a full TCP conversation, headers and all, with no host involved.
func httpOverLoopback() error {
	l, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		return err
	}
	defer l.Close()
	mux := http.NewServeMux()
	mux.HandleFunc("/hello", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprintf(w, "served %s to %s", r.URL.Path, r.RemoteAddr)
	})
	srv := &http.Server{Handler: mux}
	go srv.Serve(l)
	defer srv.Close()
	client := &http.Client{Timeout: 10 * time.Second}
	resp, err := client.Get("http://" + l.Addr().String() + "/hello")
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return err
	}
	if resp.StatusCode != http.StatusOK || !strings.HasPrefix(string(body), "served /hello to 127.0.0.1:") {
		return fmt.Errorf("status %d, body %q", resp.StatusCode, body)
	}
	fmt.Println("  http:", string(body))
	return nil
}

// resolverConfig checks the file the kernel generates from the DHCP lease.
// There is no resolver in this kernel: the lease becomes /etc/resolv.conf
// and Go's own pure-Go resolver does the rest in user space (genfs.h).
func resolverConfig() error {
	conf, err := os.ReadFile("/etc/resolv.conf")
	if err != nil {
		return fmt.Errorf("reading /etc/resolv.conf: %w", err)
	}
	text := strings.TrimSpace(string(conf))
	fmt.Printf("resolv.conf: %q\n", text)
	if !strings.HasPrefix(text, "nameserver ") {
		return fmt.Errorf("no nameserver line: %q", text)
	}
	if net.ParseIP(strings.TrimPrefix(text, "nameserver ")) == nil {
		return fmt.Errorf("the nameserver is not an address: %q", text)
	}
	if _, err := os.ReadFile("/etc/hosts"); err != nil {
		return fmt.Errorf("reading /etc/hosts: %w", err)
	}
	return nil
}

// resolveName looks a name up with Go's **own** resolver — its query
// building, its retries, its answer parsing — over the UDP sockets this
// kernel provides, and against a server the test runs. What comes back is
// checked against what that server was told to answer, so this is a claim
// about the kernel and not about the internet.
func resolveName() error {
	name, answer, server := os.Getenv(dnsNameEnv), os.Getenv(dnsAnswerEnv), os.Getenv(dnsServerEnv)
	if name == "" || answer == "" || server == "" {
		return errSkip
	}
	resolver := &net.Resolver{
		PreferGo: true,
		Dial: func(ctx context.Context, network, _ string) (net.Conn, error) {
			var d net.Dialer
			return d.DialContext(ctx, network, server)
		},
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	addrs, err := resolver.LookupHost(ctx, name)
	if err != nil {
		return fmt.Errorf("looking up %s through %s: %w", name, server, err)
	}
	fmt.Printf("%s resolved to %v\n", name, addrs)
	for _, got := range addrs {
		if got == answer {
			return nil
		}
	}
	return fmt.Errorf("%s came back as %v, want %s", name, addrs, answer)
}

func main() {
	check("network-up", waitForNetwork())
	check("udp-loopback", loopback())
	check("udp-client", udpClient())
	check("tcp-http-loopback", httpOverLoopback())
	check("tcp-client", tcpClient())
	check("udp-server", udpServer())
	check("tcp-server", tcpServer())
	check("resolv-conf", resolverConfig())
	check("dns", resolveName())
	if failed {
		os.Exit(1)
	}
	fmt.Println("ALL NET PASS")
}
