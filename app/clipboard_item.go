package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"strings"
)

// The bridge carries one clipboard item per line. A bare base64 line is
// UTF-8 text, the original protocol; a line starting with "png:" carries a
// PNG image. Older guests and launchers fail to decode the prefixed form and
// drop it, so the two sides can be upgraded independently.
type clipKind string

const (
	clipText     clipKind = "text"
	clipPNG      clipKind = "png"
	clipFiles    clipKind = "files"
	clipTransfer clipKind = "transfer"
	clipDrop     clipKind = "drop"
)

const (
	maxClipboardImageBytes = 16 << 20
	pngFramePrefix         = "png:"
)

var pngSignature = []byte("\x89PNG\r\n\x1a\n")

type clipItem struct {
	Kind clipKind
	Data []byte
}

func textItem(text string) clipItem { return clipItem{Kind: clipText, Data: []byte(text)} }
func pngItem(data []byte) clipItem  { return clipItem{Kind: clipPNG, Data: data} }

func (i clipItem) allowed() bool {
	switch i.Kind {
	case clipText:
		return clipboardTextAllowed(string(i.Data))
	case clipTransfer, clipDrop:
		var ticket fileTransferTicket
		return len(i.Data) <= 4096 && json.Unmarshal(i.Data, &ticket) == nil && validClipboardTicket(ticket)
	case clipFiles:
		_, err := inspectClipboardArchive(i.Data)
		return err == nil
	case clipPNG:
		if !bytes.HasPrefix(i.Data, pngSignature) {
			return false
		}
		_, err := clipboardPNGConfig(i.Data)
		return err == nil
	}
	return false
}

// key identifies content for loop prevention without holding a second copy
// of a large image in the sync state.
func (i clipItem) key() string {
	if i.Kind == clipText {
		return "text:" + string(i.Data)
	}
	sum := sha256.Sum256(i.Data)
	return string(i.Kind) + ":" + hex.EncodeToString(sum[:])
}

func encodeClipFrame(i clipItem) string {
	line := base64.StdEncoding.EncodeToString(i.Data)
	if i.Kind == clipPNG {
		line = pngFramePrefix + line
	} else if i.Kind == clipDrop {
		line = "drop:" + line
	} else if i.Kind == clipTransfer {
		line = "transfer:" + line
	} else if i.Kind == clipFiles {
		line = "files:" + line
	}
	return line + "\n"
}

// maxClipFrameBytes bounds one incoming line: base64 expands by 4/3, plus the
// prefix and terminator.
const maxClipFrameBytes = (maxClipboardImageBytes+2)/3*4 + len("files:") + 2

func decodeClipFrame(line string) (clipItem, bool) {
	line = strings.TrimRight(line, "\r\n")
	kind := clipText
	if strings.HasPrefix(line, pngFramePrefix) {
		kind = clipPNG
		line = line[len(pngFramePrefix):]
	}
	if strings.HasPrefix(line, "files:") {
		kind = clipFiles
		line = line[len("files:"):]
	}
	if strings.HasPrefix(line, "transfer:") {
		kind = clipTransfer
		line = strings.TrimPrefix(line, "transfer:")
	}
	if strings.HasPrefix(line, "drop:") {
		kind = clipDrop
		line = strings.TrimPrefix(line, "drop:")
	}
	data, err := base64.StdEncoding.DecodeString(line)
	if err != nil {
		return clipItem{}, false
	}
	item := clipItem{Kind: kind, Data: data}
	return item, item.allowed()
}
