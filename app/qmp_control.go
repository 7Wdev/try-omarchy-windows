package main

import (
	"context"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"time"
)

// QEMU's user network lets a guest reach host loopback. Privileged QMP must
// therefore use local filesystem sockets, never the guest-accessible TCP path.
// A private role, not a TCP port reservation. Only Windows serves it.
const qmpPowerRole = -1

var qmpControlDirectory = platformQMPControlDirectory

func qmpControlName(role int) string {
	switch role {
	case qmpToolsPort:
		return "tools.sock"
	case qmpFwdPort:
		return "forward.sock"
	case qmpSupPort:
		return "supervisor.sock"
	case qmpPowerRole:
		return "power.sock"
	default:
		return ""
	}
}

func qmpControlPath(role int) (string, error) {
	name := qmpControlName(role)
	if name == "" {
		return "", fmt.Errorf("unknown QMP control role")
	}
	dir, err := qmpControlDirectory()
	if err != nil {
		return "", err
	}
	path := filepath.Join(dir, name)
	if !filepath.IsAbs(path) || len([]byte(path)) > 103 {
		return "", fmt.Errorf("Windows private control path is too long or is not absolute")
	}
	return path, nil
}

func dialQMPControl(ctx context.Context, role int) (*qmpClient, error) {
	path, err := qmpControlPath(role)
	if err != nil {
		return nil, err
	}
	conn, err := (&net.Dialer{}).DialContext(ctx, "unix", path)
	if err != nil {
		return nil, err
	}
	return newQMPClient(ctx, conn)
}

func prepareQMPControl() (string, error) {
	dir, err := qmpControlDirectory()
	if err != nil {
		return "", err
	}
	if err := validateMovePath(dir); err != nil {
		return "", err
	}
	if err := os.MkdirAll(dir, 0700); err != nil {
		return "", err
	}
	for _, role := range platformQMPControlRoles() {
		path, err := qmpControlPath(role)
		if err != nil {
			return "", err
		}
		if _, err := os.Lstat(path); os.IsNotExist(err) {
			continue
		} else if err != nil {
			return "", err
		}
		if !isQMPControlSocket(path) {
			return "", fmt.Errorf("private control path contains another file: %s", path)
		}
		conn, err := net.DialTimeout("unix", path, 300*time.Millisecond)
		if err == nil {
			conn.Close()
			return "", fmt.Errorf("an Omarchy runtime still owns its private control socket")
		}
		if !qmpConnectionRefused(err) && !os.IsNotExist(err) {
			return "", fmt.Errorf("cannot inspect the existing private control socket: %w", err)
		}
		if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
			return "", err
		}
	}
	return dir, nil
}
