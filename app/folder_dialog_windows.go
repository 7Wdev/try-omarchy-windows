//go:build windows

package main

var procCoTaskMemFree = ole32.NewProc("CoTaskMemFree")

func browseForFolder(owner uintptr, prompt string) (string, bool) {
	selected, ok, err := chooseRecoveryPath(owner, prompt, "", false, true)
	if err != nil {
		errorBox(uiTextWith("picker.error", map[string]string{"error": err.Error()}))
	}
	return selected, ok
}
