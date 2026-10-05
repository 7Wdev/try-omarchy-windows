package main

import (
	"encoding/binary"
	"unicode/utf16"
)

const maxClipboardPathBytes = 1 << 20

func parseClipboardPaths(data []byte, absolute func(string) bool) ([]string, clipboardReadStatus) {
	if len(data) > maxClipboardPathBytes {
		return nil, clipboardRejected
	}
	if len(data) < 22 {
		return nil, clipboardUnsupported
	}
	offset := uint64(binary.LittleEndian.Uint32(data))
	if offset < 20 || offset >= uint64(len(data)-1) || binary.LittleEndian.Uint32(data[16:20]) != 1 || offset%2 != 0 {
		return nil, clipboardUnsupported
	}
	var paths []string
	var chars []uint16
	for pos := int(offset); pos+1 < len(data); pos += 2 {
		c := binary.LittleEndian.Uint16(data[pos:])
		if c != 0 {
			chars = append(chars, c)
			continue
		}
		if len(chars) == 0 {
			if len(paths) == 0 {
				return nil, clipboardUnsupported
			}
			return paths, clipboardReady
		}
		path := string(utf16.Decode(chars))
		if !absolute(path) {
			return nil, clipboardUnsupported
		}
		paths = append(paths, path)
		chars = nil
		if len(paths) > clipboardTransferLimits.Entries {
			return nil, clipboardRejected
		}
	}
	return nil, clipboardUnsupported
}
