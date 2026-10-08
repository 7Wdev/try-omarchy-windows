package main

import (
	"bufio"
	"context"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
	"unicode/utf16"
)

func TestClipboardReadRetryAndFormatFallback(t *testing.T) {
	for name, status := range map[string]clipboardReadStatus{"retry": clipboardRetry, "fallback": clipboardUnsupported, "rejected": clipboardRejected} {
		t.Run(name, func(t *testing.T) {
			host, guest := net.Pipe()
			defer host.Close()
			defer guest.Close()
			reads := 0
			b := &clipBridge{pullConn: host, sequence: func() uint32 { return 7 }, readPaths: func() ([]string, clipboardReadStatus) { return nil, status }, readHost: func() (clipItem, clipboardReadStatus) { reads++; return textItem("fallback"), clipboardReady }}
			done := make(chan struct{})
			go func() { b.sendCurrentHost(host); close(done) }()
			if status == clipboardUnsupported {
				guest.SetReadDeadline(time.Now().Add(time.Second))
				line, err := bufio.NewReader(guest).ReadString('\n')
				if err != nil || line != encodeClipFrame(textItem("fallback")) {
					t.Fatal(line, err)
				}
			}
			<-done
			if status == clipboardRetry {
				if b.lastSequenceKnown || reads != 0 {
					t.Fatal("transient read consumed sequence or fell back")
				}
				b.readPaths = nil
				b.readHost = func() (clipItem, clipboardReadStatus) { return clipItem{}, clipboardRetry }
				b.sendCurrentHost(host)
				if b.lastSequenceKnown {
					t.Fatal("host read failure consumed sequence")
				}
				b.readHost = func() (clipItem, clipboardReadStatus) { return textItem("retried"), clipboardReady }
				done2 := make(chan struct{})
				go func() { b.sendCurrentHost(host); close(done2) }()
				guest.SetReadDeadline(time.Now().Add(time.Second))
				if _, err := bufio.NewReader(guest).ReadString('\n'); err != nil {
					t.Fatal(err)
				}
				<-done2
				if b.lastSequence != 7 || !b.lastSequenceKnown {
					t.Fatal("same sequence did not retry")
				}
			} else if !b.lastSequenceKnown || reads != map[clipboardReadStatus]int{clipboardUnsupported: 1, clipboardRejected: 0}[status] {
				t.Fatal("incorrect format decision", reads)
			}
		})
	}
}

func TestClipboardBlockedArchiveAllowsTextAndReconnect(t *testing.T) {
	service := newFileTransferService(t.TempDir(), clipboardTransferLimits)
	defer service.Close()
	service.ioSlot <- struct{}{} // Hold archive preparation before any filesystem work.
	source := filepath.Join(t.TempDir(), "file")
	os.WriteFile(source, []byte("data"), 0600)
	var seq atomic.Uint32
	seq.Store(1)
	var files atomic.Bool
	files.Store(true)
	host, guest := net.Pipe()
	defer host.Close()
	defer guest.Close()
	started := make(chan struct{}, 2)
	b := &clipBridge{showTransfer: func(*transferProgress) { started <- struct{}{} }, transfers: service, pullConn: host, transferEnabled: true, sequence: seq.Load, getPaths: func() ([]string, bool) { return []string{source}, files.Load() }, getHost: func() (clipItem, bool) { return textItem("new text"), true }}
	b.sendCurrentHost(host)
	<-started
	files.Store(false)
	seq.Add(1)
	done := make(chan struct{})
	go func() { b.sendCurrentHost(host); close(done) }()
	guest.SetReadDeadline(time.Now().Add(time.Second))
	line, err := bufio.NewReader(guest).ReadString('\n')
	if err != nil || line != encodeClipFrame(textItem("new text")) {
		t.Fatal(line, err)
	}
	<-done
	// Start another stalled file selection, then establish a fresh pull session.
	files.Store(true)
	seq.Add(1)
	b.sendCurrentHost(host)
	<-started
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	b.authorizePeer = func(net.Conn) bool { return true }
	accepted := make(chan struct{}, 1)
	b.sessionStarted = func() { accepted <- struct{}{} }
	go b.acceptPull(listener)
	reconnected, err := net.Dial("tcp", listener.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer reconnected.Close()
	select {
	case <-accepted:
	case <-time.After(time.Second):
		t.Fatal("archive blocked reconnect")
	}
	files.Store(false)
	seq.Add(1)
	reconnected.SetDeadline(time.Now().Add(time.Second))
	reconnected.Write([]byte("transfer-v1\n"))
	if line, err := bufio.NewReader(reconnected).ReadString('\n'); err != nil || line != encodeClipFrame(textItem("new text")) {
		t.Fatal(line, err)
	}
	// Close while still holding the slot so cancelled preparations never touch the temp dir.
	service.Close()
	<-service.ioSlot
	b.mu.Lock()
	b.pullConn.Close()
	b.mu.Unlock()
}

func TestClipboardStalledReceiveDoesNotBlockGuestText(t *testing.T) {
	service := newFileTransferService(t.TempDir(), clipboardTransferLimits)
	defer service.Close()
	source := filepath.Join(t.TempDir(), "file")
	os.WriteFile(source, []byte("data"), 0600)
	offer, err := service.Offer(context.Background(), []string{source}, nil)
	if err != nil {
		t.Fatal(err)
	}
	host, guest := net.Pipe()
	defer host.Close()
	defer guest.Close()
	wrote := make(chan struct{}, 1)
	b := &clipBridge{transfers: service, setPaths: func([]string) bool { return true }, setHost: func(clipItem) bool { wrote <- struct{}{}; return true }}
	data, _ := json.Marshal(offer.Offer)
	done := make(chan struct{})
	go func() {
		b.receiveClipboardTransfer(host, "files-offer:"+base64.StdEncoding.EncodeToString(data)+"\n")
		close(done)
	}()
	guest.SetDeadline(time.Now().Add(time.Second))
	if _, err := bufio.NewReader(guest).ReadString('\n'); err != nil {
		t.Fatal(err)
	}
	go b.acceptGuestItem(textItem("while receiving"))
	select {
	case <-wrote:
	case <-time.After(time.Second):
		t.Fatal("completion wait blocked clipboard write")
	}
	guest.Close()
	<-done
	entries, _ := os.ReadDir(service.cache)
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), "received-") {
			t.Fatal("unfinished receive retained")
		}
	}
}

func TestClipboardRejectsPushAndPreservesPull(t *testing.T) {
	for _, push := range []bool{false, true} {
		listener, err := net.Listen("tcp", "127.0.0.1:0")
		if err != nil {
			t.Fatal(err)
		}
		old, oldPeer := net.Pipe()
		defer old.Close()
		defer oldPeer.Close()
		var writes atomic.Int32
		b := &clipBridge{pullConn: old, state: clipboardSyncState{lastSeen: "text:kept"}, authorizePeer: func(net.Conn) bool { return false }, setHost: func(clipItem) bool { writes.Add(1); return true }}
		done := make(chan struct{})
		go func() {
			if push {
				b.acceptPush(listener)
			} else {
				b.acceptPull(listener)
			}
			close(done)
		}()
		conn, err := net.Dial("tcp", listener.Addr().String())
		if err != nil {
			t.Fatal(err)
		}
		conn.SetDeadline(time.Now().Add(time.Second))
		if push {
			conn.Write([]byte(encodeClipFrame(textItem("injected"))))
		}
		data := make([]byte, 1)
		if _, err := conn.Read(data); err == nil {
			t.Fatal("rejected peer remains connected")
		}
		conn.Close()
		listener.Close()
		<-done
		b.mu.Lock()
		if b.pullConn != old || b.state.lastSeen != "text:kept" || writes.Load() != 0 {
			t.Fatal("rejected peer changed bridge")
		}
		b.mu.Unlock()
	}
}

func TestClipboardHDROPParsing(t *testing.T) {
	valid := make([]byte, 20)
	binary.LittleEndian.PutUint32(valid, 20)
	binary.LittleEndian.PutUint32(valid[16:], 1)
	for _, char := range utf16.Encode([]rune("/absolute 世界.txt\x00\x00")) {
		valid = binary.LittleEndian.AppendUint16(valid, char)
	}
	absolute := func(path string) bool { return strings.HasPrefix(path, "/") }
	paths, status := parseClipboardPaths(valid, absolute)
	if status != clipboardReady || len(paths) != 1 || paths[0] != "/absolute 世界.txt" {
		t.Fatal(paths, status)
	}
	for _, bad := range [][]byte{nil, valid[:19], valid[:len(valid)-2], append([]byte{255, 255, 255, 255}, valid[4:]...)} {
		if _, status := parseClipboardPaths(bad, absolute); status != clipboardUnsupported {
			t.Fatal("malformed paths should permit fallback", status)
		}
	}
	if _, status := parseClipboardPaths(make([]byte, maxClipboardPathBytes+1), absolute); status != clipboardRejected {
		t.Fatal("oversize should reject without fallback")
	}
}

func TestClipboardCacheSeparatesStreamingAndPreservesPublishedCopies(t *testing.T) {
	cache := t.TempDir()
	for _, dir := range []string{"streaming/received-active", "received-old", "copy-active"} {
		os.MkdirAll(filepath.Join(cache, dir), 0700)
	}
	huge, err := os.Create(filepath.Join(cache, "streaming", "received-active", "huge"))
	if err != nil {
		t.Fatal(err)
	}
	huge.Truncate(maxClipboardCacheBytes + 1)
	huge.Close()
	os.WriteFile(filepath.Join(cache, "copy-active", "keep"), []byte("keep"), 0600)
	for _, root := range []string{cache, filepath.Join(cache, "streaming")} {
		stage := filepath.Join(root, ".transfer-in-orphan")
		os.Mkdir(stage, 0700)
		os.WriteFile(filepath.Join(stage, "incoming.zip"), []byte("incomplete"), 0600)
		os.WriteFile(filepath.Join(root, ".transfer-out-orphan"), []byte("snapshot"), 0600)
		if err := cleanClipboardTransferOrphans(root); err != nil {
			t.Fatal(err)
		}
	}
	source := filepath.Join(t.TempDir(), "new")
	os.WriteFile(source, []byte("small"), 0600)
	data, err := packClipboardFiles([]string{source})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := unpackClipboardFiles(data, cache); err != nil {
		t.Fatal("streaming cache consumed legacy quota", err)
	}
	if _, err := os.Stat(filepath.Join(cache, "copy-active", "keep")); err != nil {
		t.Fatal("published clipboard path removed", err)
	}
	if _, err := os.Stat(filepath.Join(cache, "streaming", "received-active", "huge")); err != nil {
		t.Fatal("received window path removed", err)
	}
	if _, err := os.Stat(filepath.Join(cache, ".transfer-in-orphan")); !os.IsNotExist(err) {
		t.Fatal("unfinished stage retained", err)
	}
	if _, err := os.Stat(filepath.Join(cache, ".transfer-out-orphan")); !os.IsNotExist(err) {
		t.Fatal("orphan retained", err)
	}
}

func TestClipboardLateGreetingInvalidatesLegacyPreparation(t *testing.T) {
	service := newFileTransferService(t.TempDir(), clipboardTransferLimits)
	defer service.Close()
	source := filepath.Join(t.TempDir(), "file")
	os.WriteFile(source, []byte("data"), 0600)
	host, guest := net.Pipe()
	defer host.Close()
	defer guest.Close()
	ctx, cancel := context.WithCancel(context.Background())
	b := &clipBridge{transfers: service, pullConn: host, prepareCancel: cancel, revision: 1, getPaths: func() ([]string, bool) { return []string{source}, true }}
	b.finishTransferNegotiation(host, true)
	select {
	case <-ctx.Done():
	case <-time.After(time.Second):
		t.Fatal("late greeting waited for legacy preparation")
	}
	guest.SetReadDeadline(time.Now().Add(time.Second))
	line, err := bufio.NewReader(guest).ReadString('\n')
	if err != nil {
		t.Fatal(err)
	}
	item, ok := decodeClipFrame(line)
	if !ok || item.Kind != clipTransfer {
		t.Fatal("late upgrade did not replace legacy selection", item.Kind)
	}
	// A legacy result ready after negotiation must not overwrite the new offer.
	if b.sendHostItem(host, 1, 0, textItem("obsolete legacy result")) {
		t.Fatal("obsolete legacy result sent")
	}
}

func TestClipboardBlockedDropPreparationAllowsText(t *testing.T) {
	service := newFileTransferService(t.TempDir(), clipboardTransferLimits)
	defer service.Close()
	service.ioSlot <- struct{}{}
	host, guest := net.Pipe()
	defer host.Close()
	defer guest.Close()
	started := make(chan struct{}, 1)
	b := &clipBridge{showTransfer: func(*transferProgress) { started <- struct{}{} }, transfers: service, pullConn: host, transferEnabled: true, getHost: func() (clipItem, bool) { return textItem("text while dropping"), true }}
	done := make(chan error, 1)
	go func() {
		done <- b.offerDroppedFiles(droppedFiles{paths: []string{filepath.Join(t.TempDir(), "missing")}})
	}()
	<-started
	sent := make(chan struct{})
	go func() { b.sendCurrentHost(host); close(sent) }()
	guest.SetReadDeadline(time.Now().Add(time.Second))
	if _, err := bufio.NewReader(guest).ReadString('\n'); err != nil {
		t.Fatal("drop blocked text", err)
	}
	<-sent
	<-service.ioSlot
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("drop did not finish")
	}
}
