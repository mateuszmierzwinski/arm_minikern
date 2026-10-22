// Command netecho is the host side of minikern's socket tests: a TCP and a
// UDP echo service that send back exactly what they receive. It replaces
// ncat, whose --exec echo does not work on every machine, and it writes a
// ready file so a test need not sleep.
package main

import (
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"sync"
)

var (
	tcpAddr   = flag.String("tcp", "127.0.0.1:47002", "TCP address to echo on ('' disables)")
	udpAddr   = flag.String("udp", "127.0.0.1:47001", "UDP address to echo on ('' disables)")
	readyFile = flag.String("ready-file", "", "write the bound addresses here once listening")
)

func init() {
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: %s [options]\n\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "TCP and UDP echo services for the minikern socket tests.\n\nOptions:\n")
		flag.PrintDefaults()
	}
}

// serveTCP echoes each connection until the peer closes it.
func serveTCP(l net.Listener) {
	for {
		c, err := l.Accept()
		if err != nil {
			return
		}
		go func(c net.Conn) {
			defer c.Close()
			if _, err := io.Copy(c, c); err != nil {
				log.Printf("tcp echo: %v", err)
			}
		}(c)
	}
}

// serveUDP echoes each datagram back to its sender.
func serveUDP(c *net.UDPConn) {
	buf := make([]byte, 65535)
	for {
		n, from, err := c.ReadFromUDP(buf)
		if err != nil {
			return
		}
		if _, err := c.WriteToUDP(buf[:n], from); err != nil {
			log.Printf("udp echo to %s: %v", from, err)
		}
	}
}

// listen opens whichever services the flags asked for.
func listen() (string, []io.Closer, *sync.WaitGroup, error) {
	var closers []io.Closer
	var wg sync.WaitGroup
	bound := ""
	if *tcpAddr != "" {
		l, err := net.Listen("tcp4", *tcpAddr)
		if err != nil {
			return "", nil, nil, fmt.Errorf("tcp: %w", err)
		}
		closers = append(closers, l)
		bound += "tcp " + l.Addr().String() + "\n"
		wg.Add(1)
		go func() { defer wg.Done(); serveTCP(l) }()
	}
	if *udpAddr != "" {
		a, err := net.ResolveUDPAddr("udp4", *udpAddr)
		if err != nil {
			return "", nil, nil, fmt.Errorf("udp: %w", err)
		}
		c, err := net.ListenUDP("udp4", a)
		if err != nil {
			return "", nil, nil, fmt.Errorf("udp: %w", err)
		}
		closers = append(closers, c)
		bound += "udp " + c.LocalAddr().String() + "\n"
		wg.Add(1)
		go func() { defer wg.Done(); serveUDP(c) }()
	}
	return bound, closers, &wg, nil
}

func main() {
	flag.Parse()
	bound, closers, wg, err := listen()
	if err != nil {
		log.Fatalf("netecho: %v", err)
	}
	defer func() {
		for _, c := range closers {
			_ = c.Close()
		}
	}()
	fmt.Print(bound)
	if *readyFile != "" {
		if err := os.WriteFile(*readyFile, []byte(bound), 0o600); err != nil {
			log.Fatalf("netecho: %v", err)
		}
	}
	wg.Wait()
}
