package main

import (
	"encoding/binary"
	"fmt"
	"io"
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
	ext4Magic         = 0xEF53
	ext4NeedsRecovery = 0x4
)

type installReadiness struct {
	Portable    bool
	DiskMissing bool
	Running     bool
	// Unclean means the trial's filesystem was not shut down cleanly. The
	// importer copes, but the last changes are only in its journal.
	Unclean          bool
	FastStartup      bool
	FastStartupKnown bool
	SystemDrive      string
	SystemFree       int64
}

type installProbes struct {
	diskLocked  func(path string) bool
	fastStartup func() (on, known bool)
	freeBytes   func(path string) (int64, error)
	systemDrive func() string
}

// ext4Unclean reports whether a raw ext4 image still needs journal recovery.
// ok is false when the file is not an ext4 filesystem this check understands.
func ext4Unclean(r io.ReaderAt) (unclean, ok bool) {
	block := make([]byte, 1024)
	if _, err := r.ReadAt(block, 1024); err != nil {
		return false, false
	}
	if binary.LittleEndian.Uint16(block[0x38:]) != ext4Magic {
		return false, false
	}
	state := binary.LittleEndian.Uint16(block[0x3A:])
	incompat := binary.LittleEndian.Uint32(block[0x60:])
	return incompat&ext4NeedsRecovery != 0 || state&1 == 0, true
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
	} else if file, err := os.Open(disk); err == nil {
		r.Unclean, _ = ext4Unclean(file)
		file.Close()
	}
	if probes.fastStartup != nil {
		r.FastStartup, r.FastStartupKnown = probes.fastStartup()
	}
	if probes.systemDrive != nil {
		r.SystemDrive = probes.systemDrive()
	}
	if r.SystemDrive != "" && probes.freeBytes != nil {
		if free, err := probes.freeBytes(r.SystemDrive + `\`); err == nil {
			r.SystemFree = free
		}
	}
	return r
}

type installAction int

const (
	installNext installAction = iota
	installRecheck
	installFastStartup
	installEncryption
	installExportGuide
	installDiskManagement
	installDualBootGuide
	installMigrationGuide
	installDone
)

type installButton struct {
	label  string
	action installAction
}

// installChecklist is the first page: what has to be true before installing.
func installChecklist(r installReadiness) (string, []installButton) {
	if r.Portable {
		return "This is a portable Try Omarchy. Omarchy cannot read a portable disk directly, " +
				"so move your setup with an export instead:\n\n" +
				"1. Start Omarchy from this portable copy.\n" +
				"2. Open a terminal (Super+Enter) and run try-omarchy-export.\n" +
				"3. Install Omarchy, then run import.sh from the export.\n\n" +
				"The guide has the details.",
			[]installButton{{"Open the guide", installExportGuide}, {"Close", installDone}}
	}
	if r.DiskMissing {
		return "This installation has no Omarchy disk yet. Start Omarchy once and set it up, " +
				"then come back here when you are ready to install it for real.",
			[]installButton{{"Close", installDone}}
	}
	var b strings.Builder
	b.WriteString("Install Omarchy next to Windows and keep what you set up here: after installing, " +
		"one command in the new Omarchy brings this trial over. Keep Try Omarchy installed until " +
		"then, since uninstalling it deletes the trial.\n\n")
	mark := func(done bool) string {
		if done {
			return "Done: "
		}
		return "To do: "
	}
	if r.Running {
		b.WriteString(mark(false) + "shut Omarchy down, from its menu (System > Shutdown) or with " +
			"Shut down Omarchy in the tray.\n")
	} else {
		b.WriteString(mark(true) + "Omarchy is shut down.\n")
		if r.Unclean {
			b.WriteString("   It was not shut down cleanly last time. That is fine, but starting it " +
				"once and shutting it down from its menu saves your latest changes properly.\n")
		}
	}
	switch {
	case !r.FastStartupKnown:
		b.WriteString("Check: turn off Fast Startup in Control Panel > Power Options > Choose what " +
			"the power buttons do.\n")
	case r.FastStartup:
		b.WriteString(mark(false) + "turn off Fast Startup. With it on, Windows never fully shuts " +
			"down and Omarchy cannot read your files safely.\n")
	default:
		b.WriteString(mark(true) + "Fast Startup is off.\n")
	}
	b.WriteString("Check: turn off BitLocker (Device encryption). The Omarchy installer needs it off, " +
		"and it lets Omarchy read this drive.\n")
	if r.SystemFree >= 0 {
		b.WriteString(fmt.Sprintf("Then: make room by shrinking %s in Disk Management. It has %s free "+
			"now.\n", r.SystemDrive, formatGiB(r.SystemFree)))
	}
	var buttons []installButton
	if r.FastStartup {
		buttons = append(buttons, installButton{"Turn off Fast Startup", installFastStartup})
	}
	// The first button is the highlighted one, so it is the next thing to do.
	if r.Running {
		buttons = append(buttons, installButton{"Check again", installRecheck})
	} else {
		buttons = append(buttons, installButton{"Next", installNext})
	}
	buttons = append(buttons, installButton{"Encryption settings", installEncryption},
		installButton{"Close", installDone})
	return b.String(), buttons
}

// installSteps is the second page: how to install and what to run afterwards.
func installSteps() (string, []installButton) {
	body := "1. Open Disk Management, right-click " + systemDriveLabel() + " and choose Shrink " +
		"Volume. The space you free becomes Omarchy's.\n" +
		"2. Follow the Omarchy manual's dual boot guide to make a USB installer and install " +
		"Omarchy into the free space. Omarchy then starts by default; run limine-scan in " +
		"Omarchy to add Windows to its boot menu.\n" +
		"3. Start Omarchy, open a terminal (Super+Enter) and run:\n\n" +
		importCommand + "\n\n" +
		"It finds this trial on the Windows drive and asks what to bring over. The same command " +
		"is in the migration guide, which you can open in the new Omarchy's browser."
	return body, []installButton{
		{"Open Disk Management", installDiskManagement},
		{"Dual boot guide", installDualBootGuide},
		{"Migration guide", installMigrationGuide},
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
