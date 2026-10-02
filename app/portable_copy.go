package main

import (
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
)

// Convert directly from a validated raw or QCOW2 source, including its explicitly
// authenticated factory image. Neither format needs an intermediate raw copy.
func stagePortableData(dir, data, tool string, report backupProgress) error {
	for _, name := range []string{payloadUpdateStateFilename, updateStateFilename} {
		if _, err := os.Lstat(filepath.Join(dir, name)); !os.IsNotExist(err) {
			return uiError(uiText("error.portable.pending_update"), nil)
		}
	}
	disk, err := inspectInstallationDisk(dir)
	if err != nil {
		return err
	}
	return stageDirectPortableData(dir, data, disk, tool, report)
}

func stageDirectPortableData(dir, data string, disk installationDisk, tool string, report backupProgress) error {
	source, err := openBackupDisk(disk.Path)
	if err != nil {
		return uiError(uiTextWith("error.portable.close_first", map[string]string{"error": err.Error()}), err)
	}
	defer source.Close()
	verifyBacking := func() error {
		if disk.Backing == "" {
			return nil
		}
		ok, err := verifyFileSHA256(disk.Backing, disk.BackingSHA256, nil)
		if err != nil {
			return err
		}
		if !ok {
			return fmt.Errorf("portable factory image checksum mismatch")
		}
		return nil
	}
	if err := verifyBacking(); err != nil {
		return err
	}
	file := func(name string) map[string]any { return map[string]any{"driver": "file", "filename": name} }
	input := map[string]any{"driver": disk.Format, "file": file(disk.Path)}
	if disk.Format == "qcow2" {
		input["backing"] = nil
		if disk.Backing != "" {
			input["backing"] = map[string]any{"driver": "raw", "file": file(disk.Backing)}
		}
	}
	descriptor, err := json.Marshal(input)
	if err != nil {
		return err
	}
	inputName := "json:" + string(descriptor)
	// Use the backup allowlist, excluding checkpoints, retained recovery disks,
	// host-specific state and unrelated files. The inventory hashes every file
	// and budgets nonzero 64 KiB blocks rather than virtual disk capacity.
	files, required, err := inventoryMoveFiltered(dir, source, report, func(name string) bool {
		name = filepath.ToSlash(name)
		return name == "guest" || name == "runtime" || name == "vm" || backupNameAllowed(name)
	})
	if err != nil {
		return err
	}
	seen := map[string]bool{}
	if disk.Format == "qcow2" {
		// The validated disk is converted separately, never copied as a file.
		seen["vm/disk.raw"] = true
	}
	for _, entry := range files {
		if !entry.Directory {
			seen[filepath.ToSlash(entry.Name)] = true
		}
	}
	if err := requiredBackupFiles(seen); err != nil {
		return err
	}
	// For raw input, add metadata to the inventoried nonzero data blocks.
	// For QCOW2 input, measure allocated clusters including the backing image.
	// Fix the destination cluster size to match the allocation estimate.
	measureArgs := []string{"measure", "--output=json", "-O", "qcow2", "-o", "cluster_size=65536"}
	if disk.Format == "raw" {
		measureArgs = append(measureArgs, "--size", fmt.Sprint(disk.VirtualBytes))
	} else {
		// qemu-img needs to open the source to count allocated QCOW2 clusters.
		if err := source.Close(); err != nil {
			return err
		}
		measureArgs = append(measureArgs, inputName)
	}
	cmd := exec.CommandContext(setupContext(), tool, measureArgs...)
	configureDiskTool(cmd)
	var detail diskToolErrors
	cmd.Stderr = &detail
	output, err := cmd.Output()
	if err != nil {
		if setupCancelled() {
			return errSetupCancelled
		}
		return fmt.Errorf("measuring portable disk: %w: %s", err, detail.String())
	}
	var measurement struct {
		FullyAllocated int64 `json:"fully-allocated"`
		Required       int64 `json:"required"`
	}
	if err := json.Unmarshal(output, &measurement); err != nil || measurement.FullyAllocated < disk.VirtualBytes || measurement.FullyAllocated > backupMaxBytes {
		return fmt.Errorf("invalid portable disk size estimate")
	}
	if disk.Format == "raw" {
		required += measurement.FullyAllocated - disk.VirtualBytes
	} else {
		if measurement.Required <= 0 || measurement.Required > measurement.FullyAllocated {
			return fmt.Errorf("invalid allocated portable disk estimate")
		}
		required += measurement.Required
	}
	if err := requireDiskSpace(filepath.Dir(data), required); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Join(data, "vm"), 0700); err != nil {
		return err
	}
	for _, entry := range files {
		target := filepath.Join(data, entry.Name)
		if entry.Directory {
			if err := os.MkdirAll(target, 0700); err != nil {
				return err
			}
			continue
		}
		if filepath.ToSlash(entry.Name) == "vm/disk.raw" {
			continue
		}
		if err := copyMoveFile(dir, target, entry, source, report); err != nil {
			return err
		}
	}
	// Windows' exclusive source handle must close before qemu-img can open it.
	// The caller retains the installation lifecycle lock; QEMU also takes image
	// locks for conversion and comparison. No command writes the source disk.
	if disk.Format == "raw" {
		if err := source.Close(); err != nil {
			return err
		}
	}
	portable := filepath.Join(data, "vm", "disk.qcow2")
	if report != nil {
		report(0, disk.VirtualBytes, progressPhase(uiText("progress.verifying_portable_disk")))
	}
	for _, args := range [][]string{
		{"convert", "-O", "qcow2", "-o", "cluster_size=65536", inputName, portable},
		{"compare", "-F", "qcow2", inputName, portable},
	} {
		if err := checkSetupCancelled(); err != nil {
			return err
		}
		cmd := exec.CommandContext(setupContext(), tool, args...)
		configureDiskTool(cmd)
		var detail diskToolErrors
		cmd.Stderr = &detail
		if err := cmd.Run(); err != nil {
			if setupCancelled() {
				return errSetupCancelled
			}
			return fmt.Errorf("creating portable disk: %w: %s", err, detail.String())
		}
	}
	f, err := os.OpenFile(portable, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	err = f.Sync()
	f.Close()
	if err != nil {
		return err
	}
	converted, err := inspectInstallationDisk(data)
	if err != nil {
		return err
	}
	if converted.Backing != "" || converted.VirtualBytes != (disk.VirtualBytes+511)/512*512 {
		return fmt.Errorf("portable disk has unexpected size or dependencies")
	}
	return verifyBacking()
}
