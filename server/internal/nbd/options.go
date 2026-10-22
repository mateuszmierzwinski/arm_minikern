package nbd

import (
	"errors"
	"fmt"
)

// The handshake options, one struct each (master Rule 6.3), so a test can
// drive NBD_OPT_GO without going through a whole session.

// handshake is the option phase's state: what the client has agreed to, and
// what it has been given so far.
type handshake struct {
	conn     Conn
	noZeroes bool
	volume   Volume
	info     ExportInfo
}

// newOptionHandlers builds the dispatch table. NBD_OPT_INFO and NBD_OPT_GO
// differ in one thing — whether the handshake ends — so they share a
// handler parameterised by that (master Rule 5.1).
func newOptionHandlers(s *service) map[uint32]optionHandler {
	return map[uint32]optionHandler{
		optExportName: &exportNameOption{store: s.store},
		optGo:         &goOption{store: s.store, log: s.log, enterTransmission: true},
		optInfo:       &goOption{store: s.store, log: s.log, enterTransmission: false},
		optList:       &listOption{store: s.store},
		optAbort:      &abortOption{},
	}
}

// exportNameOption is the old way in: the name is the whole payload and
// there is no reply unless something is wrong, so a client that asks for a
// name we do not have sees the connection close.
type exportNameOption struct {
	store VolumeStore
}

func (o *exportNameOption) Handle(h *handshake, opt Option) (bool, error) {
	name, err := exportNamePayload(opt.Payload)
	if err != nil {
		return false, err
	}
	vol, err := o.store.Open(name)
	if err != nil {
		return false, fmt.Errorf("export %q: %w", name, err)
	}
	h.volume = vol
	h.info = infoFor(name, vol)
	return true, writeExportNameReply(h.conn, h.info, h.noZeroes)
}

// goOption is NBD_OPT_GO and NBD_OPT_INFO: it describes the export, and for
// GO it also ends the handshake. This is what our own client uses, because
// a missing export comes back as an error reply rather than a dead socket.
type goOption struct {
	store             VolumeStore
	log               Logger
	enterTransmission bool
}

func (o *goOption) Handle(h *handshake, opt Option) (bool, error) {
	name, err := goPayload(opt.Payload)
	if err != nil {
		return false, err
	}
	vol, err := o.store.Open(name)
	if err != nil {
		o.log.Printf("nbd: %s asked for export %q: %v", h.conn.RemoteName(), name, err)
		return false, o.refuse(h, err)
	}
	info := infoFor(name, vol)
	if err := o.describe(h, info); err != nil {
		_ = vol.Close()
		return false, err
	}
	if !o.enterTransmission {
		return false, vol.Close()
	}
	h.volume, h.info = vol, info
	return true, nil
}

// refuse answers with the reply type that says why and **keeps the
// handshake open**, which is the whole reason NBD_OPT_GO has error replies
// at all: a client that asks for an export it cannot have must be able to
// ask for another, or to list what there is. Ending the connection here
// instead broke `nbdinfo --list`, because it probes the default export
// before it asks for the list.
func (o *goOption) refuse(h *handshake, cause error) error {
	replyType := uint32(repErrUnknown)
	if errors.Is(cause, ErrBadName) {
		replyType = repErrInvalid
	}
	return writeOptionReply(h.conn, o.option(), replyType, nil)
}

// option is the option number this handler is answering for, which the
// reply must echo.
func (o *goOption) option() uint32 {
	if o.enterTransmission {
		return optGo
	}
	return optInfo
}

// describe sends the information replies and the ack that closes the
// option. The export's size comes first because it is the one a client
// cannot proceed without.
func (o *goOption) describe(h *handshake, info ExportInfo) error {
	option := o.option()
	for _, body := range [][]byte{infoExportBody(info), infoBlockSizeBody(), infoNameBody(info.Name)} {
		if err := writeOptionReply(h.conn, option, repInfo, body); err != nil {
			return err
		}
	}
	return writeOptionReply(h.conn, option, repAck, nil)
}

// listOption names the exports. It lists what the store offers and nothing
// else: the names are the store's own, so no path ever reaches a client.
type listOption struct {
	store VolumeStore
}

func (o *listOption) Handle(h *handshake, opt Option) (bool, error) {
	if len(opt.Payload) != 0 {
		return false, writeOptionReply(h.conn, optList, repErrInvalid, nil)
	}
	for _, name := range o.store.List() {
		body := make([]byte, 4+len(name))
		be.PutUint32(body, uint32(len(name)))
		copy(body[4:], name)
		if err := writeOptionReply(h.conn, optList, repServer, body); err != nil {
			return false, err
		}
	}
	return false, writeOptionReply(h.conn, optList, repAck, nil)
}

// abortOption is a client that changed its mind: acknowledge and close.
type abortOption struct{}

func (o *abortOption) Handle(h *handshake, _ Option) (bool, error) {
	if err := writeOptionReply(h.conn, optAbort, repAck, nil); err != nil {
		return false, err
	}
	return false, ErrClosed
}
