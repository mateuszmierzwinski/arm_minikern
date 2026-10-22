// netbootsrv is the netboot server, running on a board.
//
// It exists for one test: proving that discovery works between two machines
// on a segment of their own, rather than through an emulator's address
// translation. One guest runs this, the other netboots from it, and nothing
// on the host takes part in the conversation — their own stacks, their own
// protocol, over a cable with nothing else on it.
//
// It takes no flags, because an init here takes no arguments: the image it
// serves is in its own root filesystem, which is all the configuration it
// needs. Everything else is the same server the host runs, from the same
// packages — a second implementation would prove nothing.
package main

import (
	"context"
	"fmt"
	"log"
	"math/rand"
	"os"
	"time"

	"github.com/mateuszmierzwinski/arm_minikern/server/internal/adapters/imagestore"
	"github.com/mateuszmierzwinski/arm_minikern/server/internal/adapters/udptransport"
	"github.com/mateuszmierzwinski/arm_minikern/server/internal/mknb"
)

// Where its own root filesystem keeps the image to serve.
const images = "/srv"

type clock struct{}

func (clock) Now() time.Time { return time.Now() }

type tokens struct{}

func (tokens) Token() uint32 { return rand.Uint32() }

func main() {
	entries, err := os.ReadDir(images)
	if err != nil {
		fmt.Println("netbootsrv: cannot read", images, err)
		os.Exit(1)
	}
	for _, e := range entries {
		fmt.Println("netbootsrv: image", e.Name())
	}
	conn, err := udptransport.Listen(fmt.Sprintf(":%d", mknb.Port))
	if err != nil {
		fmt.Println("netbootsrv: cannot listen:", err)
		os.Exit(1)
	}
	defer func() { _ = conn.Close() }()
	service := mknb.NewService(imagestore.New(images), conn, clock{}, tokens{},
		log.New(os.Stdout, "netbootsrv: ", 0), mknb.Config{MaxSessions: 4})
	fmt.Println("netbootsrv: listening on", conn.LocalAddr())
	// It serves until the test stops the machine. Exiting on its own would
	// race the client's last block, and an init that exits powers the board
	// off — which would take the server away mid-transfer.
	if err := conn.Serve(context.Background(), service); err != nil {
		fmt.Println("netbootsrv: stopped:", err)
	}
}
