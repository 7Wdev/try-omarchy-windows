//go:build windows

package main

import (
	"fmt"
	"net"
	"os"
)

func runClipboardBridge() {
	cache, err := clipboardFilesCache()
	if err != nil {
		fatal(uiTextWith("fatal.file_transfers", map[string]string{"error": err.Error()}))
	}
	if err := validateMovePath(cache); err != nil {
		fatal(uiTextWith("fatal.file_transfers", map[string]string{"error": err.Error()}))
	}
	if err := os.MkdirAll(cache, 0700); err != nil {
		fatal(uiTextWith("fatal.file_transfers", map[string]string{"error": err.Error()}))
	}
	transfers := newFileTransferService(cache, clipboardTransferLimits)
	listener, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", transferPort))
	if err != nil {
		fatal(uiTextWith("fatal.port.file_transfer", map[string]string{"port": fmt.Sprint(transferPort)}))
	}
	go func() {
		if err := transfers.Serve(listener); err != nil {
			logf("file transfers: %v", err)
		}
	}()
	b := &clipBridge{dropRequests: make(chan droppedFiles, 8), setDropPaths: func(paths []string) bool { go showFileDropWindow(paths); return true }, transferError: reportTransferError, showTransfer: showTransferProgress, transfers: transfers, getPaths: clipboardGetFilePaths, setPaths: clipboardSetFilePaths, getHost: clipboardGetItem, setHost: clipboardSetItem, sequence: clipboardSequence, sessionStarted: desktopSessionStarted}
	// These listeners double as the single-instance check: a second copy of
	// the app (or a leftover QEMU on our QMP ports) must fail loudly, not
	// die 30 seconds later with an inscrutable QEMU port error.
	push, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", clipPushPort))
	if err != nil {
		fatal(uiTextWith("fatal.already_running", map[string]string{"port": fmt.Sprint(clipPushPort)}))
	}
	pull, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", clipPullPort))
	if err != nil {
		fatal(uiTextWith("fatal.already_running", map[string]string{"port": fmt.Sprint(clipPullPort)}))
	}
	logf("clipboard: guest->host on %d, host->guest on %d", clipPushPort, clipPullPort)

	desktopClipboard.Store(b)
	go func() {
		for dropped := range b.dropRequests {
			if err := b.offerDroppedFiles(dropped); err != nil {
				b.transferError(err)
			}
		}
	}()
	go b.acceptPush(push)
	go b.acceptPull(pull)
	go b.pollHost()
}
