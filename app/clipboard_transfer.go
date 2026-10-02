package main

import (
	"bufio"
	"context"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"time"
)

var clipboardTransferLimits = fileTransferLimits{Entries: 10000, Bytes: 100 << 30, ArchiveBytes: 100 << 30}

func validClipboardTicket(ticket fileTransferTicket) bool {
	id, e1 := hex.DecodeString(ticket.ID)
	token, e2 := hex.DecodeString(ticket.Token)
	return e1 == nil && e2 == nil && len(id) == 16 && len(token) == 32 && ticket.Direction == "download" && ticket.Offer.valid(clipboardTransferLimits)
}

// The clipboard selection authorizes a copy into a private cache, never a path
// supplied by the guest. Publish CF_HDROP only after the upload is verified.
func (b *clipBridge) receiveClipboardTransfer(conn net.Conn, line string) {
	drop := strings.HasPrefix(line, "drop-offer:")
	publish := b.setPaths
	if drop {
		publish = b.setDropPaths
	}
	if len(line) > 4096 || publish == nil {
		return
	}
	data, err := base64.StdEncoding.DecodeString(strings.TrimSpace(strings.TrimPrefix(strings.TrimPrefix(line, "files-offer:"), "drop-offer:")))
	var offer fileTransferOffer
	if err != nil || json.Unmarshal(data, &offer) != nil || !offer.valid(b.transfers.limits) {
		return
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	var sequence uint32
	if b.sequence != nil {
		sequence = b.sequence()
	}
	destination := filepath.Join(b.transfers.cache, "received-"+randomTransferToken(16))
	ticket, err := b.transfers.AcceptReceive(offer, destination)
	if err != nil {
		return
	}
	defer b.transfers.Cancel(ticket.ID)
	progress := b.progress(uiText("transfer.receiving_from_omarchy"))
	defer progress.finish()
	conn.SetDeadline(time.Now().Add(2 * time.Hour))
	stopCancel := context.AfterFunc(progress.ctx, func() { b.transfers.Cancel(ticket.ID); conn.SetDeadline(time.Now()) })
	defer stopCancel()
	go monitorClipboardTransfer(progress, b.transfers, ticket.ID)
	if err := json.NewEncoder(conn).Encode(ticket); err != nil {
		return
	}
	done, err := bufio.NewReader(io.LimitReader(conn, 64)).ReadString('\n')
	if err != nil || done != "complete\n" {
		return
	}
	status, ok := b.transfers.Status(ticket.ID)
	if !ok || status.State != "completed" {
		return
	}
	// A newer Windows selection wins over a transfer that was already underway.
	if !drop && b.sequence != nil && b.sequence() != sequence {
		fmt.Fprintln(conn, "superseded")
		return
	}
	entries, err := os.ReadDir(destination)
	if err != nil || len(entries) == 0 {
		return
	}
	paths := make([]string, 0, len(entries))
	for _, entry := range entries {
		paths = append(paths, filepath.Join(destination, entry.Name()))
	}
	if !publish(paths) {
		return
	}
	if !drop {
		b.state = clipboardSyncState{}
		if b.sequence != nil {
			b.lastSequence = b.sequence()
		}
	}
	fmt.Fprintln(conn, "complete")
}
