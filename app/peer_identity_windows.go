//go:build windows

package main

import (
	"net"
	"syscall"
	"unsafe"
)

var procProcessIdToSessionId = kernel32.NewProc("ProcessIdToSessionId")

const processQueryLimitedInformation = 0x1000

// connectionFromSameUser reports whether the other end of a loopback
// connection is a process of this Windows user in this session. The -open-url
// client checks the listener with it before sending a link; the listener
// checks its one peer lookup with processFromSameUser. Any lookup that fails
// refuses.
func connectionFromSameUser(conn net.Conn) bool {
	pid, err := loopbackPeerPID(conn)
	return err == nil && processFromSameUser(pid)
}

func processFromSameUser(pid uint32) bool {
	if pid == 0 {
		return false
	}
	peerSession, ok := processSession(pid)
	if !ok {
		return false
	}
	ownSession, ok := processSession(uint32(syscall.Getpid()))
	if !ok || peerSession != ownSession {
		return false
	}
	peerUser, ok := processUserSID(pid)
	if !ok {
		return false
	}
	ownUser, ok := processUserSID(uint32(syscall.Getpid()))
	return ok && peerUser == ownUser
}

func processSession(pid uint32) (uint32, bool) {
	var session uint32
	r, _, _ := procProcessIdToSessionId.Call(uintptr(pid), uintptr(unsafe.Pointer(&session)))
	return session, r != 0
}

func processUserSID(pid uint32) (string, bool) {
	process, err := syscall.OpenProcess(processQueryLimitedInformation, false, pid)
	if err != nil {
		return "", false
	}
	defer syscall.CloseHandle(process)
	var token syscall.Token
	if err := syscall.OpenProcessToken(process, syscall.TOKEN_QUERY, &token); err != nil {
		return "", false
	}
	defer token.Close()
	user, err := token.GetTokenUser()
	if err != nil {
		return "", false
	}
	sid, err := user.User.Sid.String()
	return sid, err == nil && sid != ""
}
