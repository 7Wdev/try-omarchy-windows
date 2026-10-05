package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net"
	"strings"
	"sync"
	"time"
)

// Host side of the two-way text clipboard bridge, ported from
// scripts/clipboard-bridge.ps1. The guest daemon (scripts/guest/
// clipboard-bridge.sh, baked into the image) reaches these listeners as
// 10.0.2.2 over QEMU user-mode networking:
//   push port (4448): guest -> host, one line per change, then close
//   pull port (4449): host -> guest, one persistent connection, one line per change
// A line is base64(UTF-8 text), or "png:" + base64(PNG) for an image.
// Loop prevention: each side skips content it just received. Lines are LF-only;
// CRLF corrupts the guest's base64 -d.

type clipBridge struct {
	mu                sync.Mutex
	clipboardMu       sync.Mutex
	writeMu           sync.Mutex
	revision          uint64
	prepareCancel     context.CancelFunc
	authorizePeer     func(net.Conn) bool
	readHost          func() (clipItem, clipboardReadStatus)
	readPaths         func() ([]string, clipboardReadStatus)
	lastSequenceKnown bool
	state             clipboardSyncState
	pullConn          net.Conn
	getHost           func() (clipItem, bool)
	setHost           func(clipItem) bool
	// sequence reports the Windows clipboard sequence number when available,
	// so an unchanged clipboard (which may hold a large image) is not read
	// and converted on every poll.
	sequence     func() uint32
	lastSequence uint32
	// The Windows clipboard sequence when the guest connected. Files copied
	// before Omarchy started may have moved or been deleted since; failing
	// to offer those is not worth an error the user never asked for.
	connectSequence      uint32
	connectSequenceKnown bool
	transfers            *fileTransferService
	transferEnabled      bool
	transferNegotiating  bool
	outgoingTransfer     string
	showTransfer         func(*transferProgress)
	transferError        func(error)
	getPaths             func() ([]string, bool)
	setPaths             func([]string) bool
	setDropPaths         func([]string) bool
	dropRequests         chan droppedFiles
	// sessionStarted runs when the guest connects: the bridge starts with
	// the desktop session.
	sessionStarted func()
}

func (b *clipBridge) acceptPush(l net.Listener) {
	slots := make(chan struct{}, 4)
	for {
		c, err := l.Accept()
		if err != nil {
			return
		}
		if b.authorizePeer == nil || !b.authorizePeer(c) {
			c.Close()
			continue
		}
		select {
		case slots <- struct{}{}:
		default:
			c.Close()
			continue
		}
		go func(c net.Conn) {
			defer func() { <-slots }()
			defer c.Close()
			// A compromised or broken guest must not make the Windows launcher
			// allocate an unbounded line. Base64 expands data by at most 4/3.
			c.SetReadDeadline(time.Now().Add(10 * time.Second))
			line, err := bufio.NewReader(io.LimitReader(c, int64(maxClipFrameBytes))).ReadString('\n')
			if err != nil || !strings.HasSuffix(line, "\n") {
				return
			}
			if (strings.HasPrefix(line, "files-offer:") || strings.HasPrefix(line, "drop-offer:")) && b.transfers != nil {
				b.receiveClipboardTransfer(c, line)
				return
			}
			item, ok := decodeClipFrame(line)
			if !ok {
				return
			}
			b.acceptGuestItem(item)
		}(c)
	}
}

func (b *clipBridge) acceptPull(l net.Listener) {
	for {
		c, err := l.Accept()
		if err != nil {
			return
		}
		if b.authorizePeer == nil || !b.authorizePeer(c) {
			c.Close()
			continue
		}
		b.mu.Lock()
		old := b.pullConn
		cancel, outgoing := b.invalidatePreparationLocked()
		b.pullConn = c
		b.transferEnabled = false
		b.transferNegotiating = true
		b.state = clipboardSyncState{}
		b.lastSequenceKnown = false
		b.connectSequenceKnown = b.sequence != nil
		if b.connectSequenceKnown {
			b.connectSequence = b.sequence()
		}
		b.mu.Unlock()
		if old != nil {
			old.Close()
		}
		b.cancelPreparation(cancel, outgoing)

		logf("clipboard: guest connected")
		if b.sessionStarted != nil {
			b.sessionStarted()
		}
		// Legacy guests never send a greeting. Release their initial file
		// selection after a short grace period, but keep listening so a slow
		// current guest can still negotiate streaming on this connection.
		timer := time.AfterFunc(300*time.Millisecond, func() {
			b.finishTransferNegotiation(c, false)
		})
		go func() {
			hello, _ := bufio.NewReader(io.LimitReader(c, 64)).ReadString('\n')
			timer.Stop()
			b.finishTransferNegotiation(c, hello == "transfer-v1\n")
		}()
		go b.sendCurrentHost(c)
	}
}

// A late greeting may upgrade only the connection that sent it.
func (b *clipBridge) finishTransferNegotiation(c net.Conn, capable bool) {
	b.mu.Lock()
	if b.pullConn != c || (!b.transferNegotiating && (!capable || b.transferEnabled)) {
		b.mu.Unlock()
		return
	}
	cancel, outgoing := b.invalidatePreparationLocked()
	b.transferNegotiating = false
	b.transferEnabled = capable && b.transfers != nil
	b.lastSequenceKnown = false
	b.mu.Unlock()
	b.cancelPreparation(cancel, outgoing)
	b.sendCurrentHost(c)
}

func (b *clipBridge) pollHost() {
	for {
		time.Sleep(400 * time.Millisecond)
		b.mu.Lock()
		conn := b.pullConn
		lastSequence := b.lastSequence
		known := b.lastSequenceKnown
		b.mu.Unlock()
		if conn == nil {
			continue
		}
		if b.sequence != nil {
			if seq := b.sequence(); known && seq == lastSequence {
				continue
			}
		}
		b.sendCurrentHost(conn)
	}
}

// Native clipboard access is serialized separately from connection state.
// Archives and network writes never hold the bridge state mutex.
func (b *clipBridge) sendCurrentHost(conn net.Conn) {
	b.clipboardMu.Lock()
	b.mu.Lock()
	if b.pullConn != conn {
		b.mu.Unlock()
		b.clipboardMu.Unlock()
		return
	}
	revision, enabled, negotiating := b.revision, b.transferEnabled, b.transferNegotiating
	b.mu.Unlock()
	var sequence uint32
	if b.sequence != nil {
		sequence = b.sequence()
	}
	paths, status := b.hostPaths()
	var cur clipItem
	if status == clipboardReady && negotiating {
		b.clipboardMu.Unlock()
		return
	}
	if status == clipboardUnsupported {
		cur, status = b.hostItem()
	}
	if status == clipboardRetry || b.sequence != nil && b.sequence() != sequence {
		b.clipboardMu.Unlock()
		return
	}
	b.mu.Lock()
	if b.pullConn != conn || b.revision != revision {
		b.mu.Unlock()
		b.clipboardMu.Unlock()
		return
	}
	if b.sequence != nil && b.lastSequenceKnown && b.lastSequence == sequence {
		b.mu.Unlock()
		b.clipboardMu.Unlock()
		return
	}
	// A successful read (or intentional rejection) consumes this sequence.
	cancel, outgoing := b.invalidatePreparationLocked()
	revision = b.revision
	b.lastSequence, b.lastSequenceKnown = sequence, b.sequence != nil
	ctx, prepareCancel := context.WithCancel(context.Background())
	if status == clipboardReady && len(paths) != 0 {
		b.prepareCancel = prepareCancel
	}
	b.mu.Unlock()
	b.clipboardMu.Unlock()
	b.cancelPreparation(cancel, outgoing)
	if status != clipboardReady {
		prepareCancel()
		return
	}
	if len(paths) != 0 {
		go b.prepareHostFiles(ctx, prepareCancel, conn, revision, sequence, enabled, paths)
		return
	}
	prepareCancel()
	b.sendHostItem(conn, revision, sequence, cur)
}

func (b *clipBridge) hostPaths() ([]string, clipboardReadStatus) {
	if b.readPaths != nil {
		return b.readPaths()
	}
	if b.getPaths != nil {
		if paths, ok := b.getPaths(); ok {
			return paths, clipboardReady
		}
	}
	return nil, clipboardUnsupported
}
func (b *clipBridge) hostItem() (clipItem, clipboardReadStatus) {
	if b.readHost != nil {
		return b.readHost()
	}
	if b.getHost != nil {
		if item, ok := b.getHost(); ok {
			return item, clipboardReady
		}
	}
	return clipItem{}, clipboardRetry
}

// Caller holds mu. Cancellation and disk cleanup happen after unlocking.
func (b *clipBridge) invalidatePreparationLocked() (context.CancelFunc, string) {
	b.revision++
	cancel, outgoing := b.prepareCancel, b.outgoingTransfer
	b.prepareCancel, b.outgoingTransfer = nil, ""
	return cancel, outgoing
}
func (b *clipBridge) cancelPreparation(cancel context.CancelFunc, outgoing string) {
	if cancel != nil {
		cancel()
	}
	if outgoing != "" && b.transfers != nil {
		b.transfers.Cancel(outgoing)
	}
}
func (b *clipBridge) selectionCurrentLocked(conn net.Conn, revision uint64, sequence uint32) bool {
	return b.pullConn == conn && b.revision == revision && (b.sequence == nil || b.sequence() == sequence)
}

func (b *clipBridge) prepareHostFiles(ctx context.Context, cancel context.CancelFunc, conn net.Conn, revision uint64, sequence uint32, enabled bool, paths []string) {
	defer cancel()
	progress := b.progress(uiText("transfer.preparing_to_omarchy"))
	stop := context.AfterFunc(ctx, progress.cancel)
	defer stop()
	var item clipItem
	var ticket fileTransferTicket
	var err error
	if enabled {
		ticket, err = b.transfers.Offer(progress.ctx, paths, progress.report)
		data, _ := json.Marshal(ticket)
		item = clipItem{Kind: clipTransfer, Data: data}
	} else {
		item.Kind = clipFiles
		item.Data, err = packClipboardFilesContext(progress.ctx, paths)
	}
	b.mu.Lock()
	current := b.selectionCurrentLocked(conn, revision, sequence)
	copiedBeforeConnect := b.connectSequenceKnown && sequence == b.connectSequence
	if current {
		b.prepareCancel = nil
	}
	if current && err == nil && enabled {
		b.outgoingTransfer = ticket.ID
	}
	b.mu.Unlock()
	if err != nil || !current {
		if ticket.ID != "" {
			b.transfers.Cancel(ticket.ID)
		}
		progress.finish()
		if current && err != nil && !errors.Is(err, context.Canceled) && !copiedBeforeConnect && b.transferError != nil {
			b.transferError(err)
		}
		return
	}
	if !b.sendHostItem(conn, revision, sequence, item) {
		if ticket.ID != "" {
			b.transfers.Cancel(ticket.ID)
		}
		progress.finish()
		return
	}
	if enabled {
		go monitorClipboardTransfer(progress, b.transfers, ticket.ID)
	} else {
		progress.finish()
	}
}

func (b *clipBridge) sendHostItem(conn net.Conn, revision uint64, sequence uint32, cur clipItem) bool {
	b.writeMu.Lock()
	defer b.writeMu.Unlock()
	b.mu.Lock()
	current := b.selectionCurrentLocked(conn, revision, sequence) && b.state.shouldSendHost(cur)
	b.mu.Unlock()
	if !current {
		return false
	}
	line := encodeClipFrame(cur)
	conn.SetWriteDeadline(time.Now().Add(20 * time.Second))
	n, err := conn.Write([]byte(line))
	b.mu.Lock()
	if err != nil || n != len(line) {
		if b.pullConn == conn {
			b.pullConn = nil
			b.lastSequenceKnown = false
		}
		b.mu.Unlock()
		conn.Close()
		return false
	}
	if b.selectionCurrentLocked(conn, revision, sequence) {
		b.state.markHostSent(cur)
	}
	b.mu.Unlock()
	return true
}

func (b *clipBridge) acceptGuestItem(item clipItem) {
	b.clipboardMu.Lock()
	defer b.clipboardMu.Unlock()
	b.mu.Lock()
	accept := b.state.shouldAcceptGuest(item)
	b.mu.Unlock()
	if !accept {
		return
	}
	if b.setHost(item) {
		b.mu.Lock()
		cancel, outgoing := b.invalidatePreparationLocked()
		b.state.markGuestAccepted(item)
		if b.sequence != nil {
			b.lastSequence, b.lastSequenceKnown = b.sequence(), true
		}
		b.mu.Unlock()
		b.cancelPreparation(cancel, outgoing)
	} else {
		logf("clipboard: could not write the Windows clipboard")
	}
}
