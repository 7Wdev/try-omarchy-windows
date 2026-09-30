package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// Installing Omarchy next to Windows: the launcher checks what would get in
// the way and explains the steps. The import itself runs on the new install
// (migrate/ in this repository), which reads vm\disk.raw straight off the
// Windows drive.

const (
	importCommand     = "curl -fsSL https://github.com/omacom/try-omarchy-windows/releases/latest/download/try-omarchy-import.sh | bash"
	dualBootGuideURL  = "https://learn.omacom.io/2/the-omarchy-manual/120/dual-boot-install"
	migrationGuideURL = "https://github.com/omacom/try-omarchy-windows/blob/master/docs/MIGRATION.md"
	exportGuideURL    = migrationGuideURL + "#replacing-windows-or-moving-to-another-computer"
)

// bitLockerState is ordered so that the drive needing the most work wins.
type bitLockerState int

const (
	bitLockerUnknown bitLockerState = iota
	bitLockerOff
	bitLockerDecrypting
	bitLockerOn
)

// bitLockerFromShell reads the System.Volume.BitLockerProtection property
// Explorer uses for its drive icons. Measured on Windows 11: 1 on, 2 off,
// 3 encrypting, 4 decrypting, 5 suspended, 6 locked. Everything but off and
// decrypting leaves the drive encrypted, which the Omarchy installer refuses.
func bitLockerFromShell(value int32) bitLockerState {
	switch value {
	case 0:
		return bitLockerUnknown
	case 2:
		return bitLockerOff
	case 4:
		return bitLockerDecrypting
	default:
		return bitLockerOn
	}
}

type installReadiness struct {
	Portable    bool
	DiskMissing bool
	Running     bool
	// ShutdownRequested is set by the walkthrough after it asked Omarchy to
	// shut down, so a slow shutdown reads as in progress.
	ShutdownRequested bool
	// FastStartup is true when it is on or cannot be read. Turning it off
	// is harmless either way.
	FastStartup    bool
	BitLocker      bitLockerState
	BitLockerDrive string
	SystemDrive    string
	SystemFree     int64
}

type installProbes struct {
	diskLocked  func(path string) bool
	fastStartup func() (on, known bool)
	bitLocker   func(drive string) bitLockerState
	freeBytes   func(path string) (int64, error)
	systemDrive func() string
}

// installDrives lists the Windows drive and, if different, the drive that
// holds the trial: the installer needs the first, the importer reads both.
func installDrives(systemDrive, dir string) []string {
	var drives []string
	for _, drive := range []string{systemDrive, filepath.VolumeName(dir)} {
		drive = strings.ToUpper(drive)
		if len(drive) != 2 || drive[1] != ':' || (len(drives) > 0 && drives[0] == drive) {
			continue
		}
		drives = append(drives, drive)
	}
	return drives
}

func assessInstallReadiness(dir string, probes installProbes) installReadiness {
	r := installReadiness{SystemFree: -1}
	vm := filepath.Join(dir, "vm")
	if _, err := os.Lstat(filepath.Join(vm, "disk.qcow2")); err == nil {
		r.Portable = true
	}
	disk := filepath.Join(vm, "disk.raw")
	if _, err := os.Lstat(disk); err != nil {
		r.DiskMissing = !r.Portable
	} else if probes.diskLocked != nil && probes.diskLocked(disk) {
		r.Running = true
	}
	if probes.fastStartup != nil {
		on, known := probes.fastStartup()
		r.FastStartup = on || !known
	}
	if probes.systemDrive != nil {
		r.SystemDrive = probes.systemDrive()
	}
	if r.SystemDrive != "" && probes.freeBytes != nil {
		if free, err := probes.freeBytes(r.SystemDrive + `\`); err == nil {
			r.SystemFree = free
		}
	}
	if probes.bitLocker != nil {
		for _, drive := range installDrives(r.SystemDrive, dir) {
			if state := probes.bitLocker(drive); state > r.BitLocker {
				r.BitLocker, r.BitLockerDrive = state, drive
			}
		}
	}
	return r
}

type installAction int

const (
	installRecheck installAction = iota
	installShutDown
	installFastStartup
	installBitLocker
	installExportGuide
	installDiskManagement
	installGuide
	installDone
)

type installButton struct {
	label  string
	action installAction
}

// installPage is what the walkthrough shows next: what still has to be done,
// each with a button that does it or opens the right place, or the install
// steps once nothing is left.
func installPage(r installReadiness) (string, []installButton) {
	if r.Portable {
		return "Omarchy can't read a portable copy's disk directly, so bring your setup over " +
				"with an export: start Omarchy from this copy, run try-omarchy-export in a " +
				"terminal, and follow the steps it prints.",
			[]installButton{{"Open the guide", installExportGuide}, {"Close", installDone}}
	}
	if r.DiskMissing {
		return "Start Omarchy and set it up first, then come back here.",
			[]installButton{{"Close", installDone}}
	}
	var todo []string
	var buttons []installButton
	if r.Running && r.ShutdownRequested {
		todo = append(todo, "Omarchy is shutting down.")
	} else if r.Running {
		todo = append(todo, "Shut down Omarchy.")
		buttons = append(buttons, installButton{"Shut down Omarchy", installShutDown})
	}
	if r.FastStartup {
		todo = append(todo, "Turn off Fast Startup, so Windows fully shuts down.")
		buttons = append(buttons, installButton{"Turn off Fast Startup", installFastStartup})
	}
	switch r.BitLocker {
	case bitLockerOn:
		todo = append(todo, "Turn off BitLocker on "+r.BitLockerDrive+". Omarchy can't install next to "+
			"an encrypted drive.")
		buttons = append(buttons, installButton{"Open BitLocker settings", installBitLocker})
	case bitLockerDecrypting:
		todo = append(todo, "Wait for "+r.BitLockerDrive+" to finish decrypting.")
	}
	if len(todo) == 0 {
		return installSteps(r)
	}
	body := "Before installing Omarchy next to Windows:\n\n• " + strings.Join(todo, "\n• ")
	return body, append(buttons, installButton{"Check again", installRecheck}, installButton{"Close", installDone})
}

// installSteps is the last page: how to install and what to run afterwards.
func installSteps(r installReadiness) (string, []installButton) {
	drive := r.SystemDrive
	if drive == "" {
		drive = "C:"
	}
	room := ""
	if r.SystemFree >= 0 {
		room = fmt.Sprintf(" It has %s free.", formatGiB(r.SystemFree))
	}
	body := "Your PC is ready for Omarchy.\n\n" +
		"1. Shrink " + drive + " in Disk Management to make room." + room + "\n" +
		"2. Install Omarchy from a USB stick into the free space. The install guide shows how.\n" +
		"3. In the new Omarchy, open a terminal (Super+Enter) and run:\n\n" +
		importCommand + "\n\n" +
		"Keep Try Omarchy installed until you've run it. Uninstalling it deletes the trial."
	return body, []installButton{
		{"Open Disk Management", installDiskManagement},
		{"Open the install guide", installGuide},
		{"Done", installDone},
	}
}

func systemDriveLabel() string {
	if drive := os.Getenv("SystemDrive"); drive != "" {
		return drive
	}
	return "C:"
}

func installButtonLabels(buttons []installButton) []string {
	labels := make([]string, len(buttons))
	for i, button := range buttons {
		labels[i] = button.label
	}
	return labels
}
