package main

const runtimeZip = "winq-emu-alpha10-portable.zip"

var (
	downloadedGuestArtifacts = []string{"guest-manifest.json", "build-spec.json", "vmlinuz-linux", "initramfs-linux.img"}
	bootGuestArtifacts       = []string{"build-spec.json", "vmlinuz-linux", "initramfs-linux.img"}
	preparedBootArtifacts    = append(append([]string{}, downloadedGuestArtifacts...), "SHA256SUMS")
	installedGuestArtifacts  = []string{"build-spec.json", "vmlinuz-linux", "initramfs-linux.img", "rootfs.ext4"}
)

// buildSpec holds the fields the launcher reads from the guest's build-spec.json.
type buildSpec struct {
	Runtime struct {
		KernelCommandLine string   `json:"kernelCommandLine"`
		OptionalDevices   []string `json:"optionalDevices"`
		Storage           struct {
			ExpandedSizeMiB int64 `json:"expandedSizeMiB"`
		} `json:"storage"`
	} `json:"runtime"`
}
