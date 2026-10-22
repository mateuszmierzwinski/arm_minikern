// dnsecho is a one-name authoritative DNS server, for the host side of the
// DNS test.
//
// The guest's resolver must be answered by something, and a test that asked
// the real internet would be a test of the internet. So this answers
// exactly one name with exactly one address, which is what makes "the
// kernel can resolve a name" a claim about the kernel.
package main

import (
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"strings"
)

var (
	listen = flag.String("listen", "0.0.0.0:0", "address to serve DNS on (port 0 picks one)")
	name   = flag.String("name", "minikern.test.", "the one name this answers for")
	answer = flag.String("answer", "10.9.8.7", "the address to answer with")
	ready  = flag.String("ready-file", "", "write the listening address here once up")
)

func init() {
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: %s [options]\n\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "A one-name DNS server, for minikern's DNS test.\n\n")
		fmt.Fprintf(os.Stderr, "Options:\n")
		flag.PrintDefaults()
	}
}

func main() {
	flag.Parse()
	addr := net.ParseIP(*answer).To4()
	if addr == nil {
		log.Fatalf("dnsecho: %q is not an IPv4 address", *answer)
	}
	conn, err := net.ListenPacket("udp4", *listen)
	if err != nil {
		log.Fatalf("dnsecho: %v", err)
	}
	defer func() { _ = conn.Close() }()
	log.Printf("dnsecho: answering %s with %s on %s", *name, *answer, conn.LocalAddr())
	if err := announce(conn.LocalAddr().String()); err != nil {
		log.Fatalf("dnsecho: %v", err)
	}
	serve(conn, addr)
}

// announce writes the address we ended up on, so a test that asked for port
// 0 knows where to point the guest.
func announce(addr string) error {
	if *ready == "" {
		return nil
	}
	return os.WriteFile(*ready, []byte(addr+"\n"), 0o600)
}

func serve(conn net.PacketConn, addr net.IP) {
	buf := make([]byte, 1500)
	for {
		n, from, err := conn.ReadFrom(buf)
		if err != nil {
			return
		}
		reply, err := respond(buf[:n], addr)
		if err != nil {
			log.Printf("dnsecho: %v from %v", err, from)
			continue
		}
		if _, err := conn.WriteTo(reply, from); err != nil {
			log.Printf("dnsecho: replying to %v: %v", from, err)
		}
	}
}

// respond builds an answer to one query. It implements just enough of RFC
// 1035 to answer an A question for one name, and refuses everything else
// rather than guessing.
func respond(query []byte, addr net.IP) ([]byte, error) {
	if len(query) < 12 {
		return nil, fmt.Errorf("a %d-byte query", len(query))
	}
	qname, end, err := readName(query, 12)
	if err != nil {
		return nil, err
	}
	if end+4 > len(query) {
		return nil, fmt.Errorf("the question runs past the message")
	}
	qtype := int(query[end])<<8 | int(query[end+1])
	reply := append([]byte(nil), query[:end+4]...)
	reply[2], reply[3] = 0x81, 0x80 // a response, recursion available
	reply[6], reply[7] = 0, 0       // answer count, filled in below
	reply[8], reply[9] = 0, 0
	reply[10], reply[11] = 0, 0
	if !strings.EqualFold(qname, *name) || qtype != 1 {
		reply[3] = 0x83 // NXDOMAIN: this server knows one name
		return reply, nil
	}
	reply[7] = 1
	reply = append(reply, 0xc0, 0x0c) // the name, as a pointer to the question
	reply = append(reply, 0, 1, 0, 1) // type A, class IN
	reply = append(reply, 0, 0, 0, 30) // thirty seconds
	reply = append(reply, 0, 4)
	return append(reply, addr...), nil
}

// readName reads a DNS name, refusing the compression pointers a query has
// no business carrying.
func readName(msg []byte, at int) (string, int, error) {
	var out strings.Builder
	for at < len(msg) {
		length := int(msg[at])
		if length == 0 {
			return out.String(), at + 1, nil
		}
		if length >= 0xc0 {
			return "", 0, fmt.Errorf("a compression pointer in a question")
		}
		if at+1+length > len(msg) {
			return "", 0, fmt.Errorf("a label runs past the message")
		}
		out.Write(msg[at+1 : at+1+length])
		out.WriteByte('.')
		at += 1 + length
	}
	return "", 0, fmt.Errorf("an unterminated name")
}
