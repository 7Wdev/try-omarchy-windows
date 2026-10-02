package main

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
)

func growPortableDisk(dir string, disk installationDisk, bytes int64) error {
	name := "qemu-img"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	return growPortableDiskWithTool(disk, bytes, filepath.Join(dir, "runtime", "bin", name), publishMoveFile)
}

// Grow a private byte-for-byte copy beside the active disk. Relative backing
// paths remain valid, and cancellation or tool failure leaves the source intact.
func growPortableDiskWithTool(disk installationDisk, bytes int64, tool string, publish func(string, string) error) error {
	if disk.Format != "qcow2" || bytes <= 0 || bytes > int64(maximumDiskGiB)<<30 {
		return fmt.Errorf("invalid portable disk capacity")
	}
	source, err := openBackupDisk(disk.Path)
	if err != nil {
		return uiError(uiTextWith("error.disk.close_first", map[string]string{"error": err.Error()}), err)
	}
	defer source.Close()
	if err := cleanupPortableGrowth(filepath.Dir(disk.Path)); err != nil {
		return err
	}
	if bytes <= disk.VirtualBytes {
		return nil
	}
	info, err := source.Stat()
	if err != nil {
		return err
	}
	parent := filepath.Dir(disk.Path)
	if err := requireDiskSpace(parent, info.Size()+diskSpaceReserve); err != nil {
		return err
	}
	stage, err := os.CreateTemp(parent, ".try-omarchy-grow-*.qcow2")
	if err != nil {
		return err
	}
	path := stage.Name()
	defer os.Remove(path)
	defer stage.Close()
	if _, err := io.Copy(stage, setupReader{source}); err != nil {
		return err
	}
	if err := stage.Sync(); err != nil {
		return err
	}
	if err := stage.Close(); err != nil {
		return err
	}
	for _, args := range [][]string{{"resize", "-f", "qcow2", path, strconv.FormatInt(bytes, 10)}, {"check", "-f", "qcow2", path}} {
		cmd := exec.CommandContext(setupContext(), tool, args...)
		configureDiskTool(cmd)
		var detail diskToolErrors
		cmd.Stderr = &detail
		if err := cmd.Run(); err != nil {
			if setupCancelled() {
				return errSetupCancelled
			}
			return fmt.Errorf("growing portable disk: %w: %s", err, detail.String())
		}
	}
	resized, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	header := make([]byte, qcow2HeaderSize)
	_, err = io.ReadFull(resized, header)
	if err == nil && (binary.BigEndian.Uint32(header[:4]) != 0x514649fb || binary.BigEndian.Uint64(header[24:32]) != uint64(bytes)) {
		err = fmt.Errorf("expanded portable disk has an unexpected size")
	}
	if err == nil {
		err = resized.Sync()
	}
	resized.Close()
	if err != nil {
		return err
	}
	if err := checkSetupCancelled(); err != nil {
		return err
	}
	// Windows keeps the source exclusively open through validation. Close it
	// only when the completed replacement is ready for atomic publication.
	if err := source.Close(); err != nil {
		return err
	}
	return publish(path, disk.Path)
}

var portableGrowthStage = regexp.MustCompile(`^\.try-omarchy-grow-[0-9]+\.qcow2$`)

// Called only while the active disk is exclusively locked.
func cleanupPortableGrowth(parent string) error {
	root, err := os.OpenRoot(parent)
	if err != nil {
		return err
	}
	defer root.Close()
	dir, err := root.Open(".")
	if err != nil {
		return err
	}
	entries, err := dir.ReadDir(-1)
	dir.Close()
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if !portableGrowthStage.MatchString(entry.Name()) {
			continue
		}
		info, err := root.Lstat(entry.Name())
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() || rejectMoveLink(filepath.Join(parent, entry.Name()), info) != nil {
			continue
		}
		if err := root.Remove(entry.Name()); err != nil {
			return err
		}
	}
	return nil
}
