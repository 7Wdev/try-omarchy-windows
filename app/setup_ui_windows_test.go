//go:build windows

package main

import (
	"context"
	"os"
	"os/exec"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

// The child owns a fresh singleton, so unrelated extraction tests cannot leave
// this test with an already-finished splash. No download or installation runs.
func TestSetupPromptsNative(t *testing.T) {
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" {
		t.Skip("requires interactive Windows")
	}
	if os.Getenv("TRYOMARCHY_SETUP_CHILD") != "1" {
		self, err := os.Executable()
		if err != nil {
			t.Fatal(err)
		}
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		cmd := exec.CommandContext(ctx, self, "-test.v", "-test.timeout=20s", "-test.run=^TestSetupPromptsNative$")
		cmd.Env = append(os.Environ(), "TRYOMARCHY_SETUP_CHILD=1")
		cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
		if output, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("prompt child: %v\n%s", err, output)
		}
		return
	}
	t.Log("creating setup window")
	ui := getUI()
	defer ui.finish()
	t.Log("setup window ready")
	class, _ := syscall.UTF16PtrFromString("TryOmarchySetup")
	window, _, _ := user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), 0)
	visible, _, _ := user32.NewProc("IsWindowVisible").Call(window)
	if window == 0 || visible == 0 {
		t.Fatal("setup window was hidden by process startup flags")
	}
	get := func(id uintptr) uintptr { h, _, _ := user32.NewProc("GetDlgItem").Call(window, id); return h }
	for _, id := range []uintptr{cancelControlID, promptOption1ID, promptOption2ID, promptContinueID} {
		var name [32]uint16
		procGetClassNameW.Call(get(id), uintptr(unsafe.Pointer(&name[0])), uintptr(len(name)))
		if syscall.UTF16ToString(name[:]) != "Button" && syscall.UTF16ToString(name[:]) != "BUTTON" {
			t.Fatalf("control %d is not a native button: %s", id, syscall.UTF16ToString(name[:]))
		}
	}
	choose := func(kind setupPromptKind, second bool) setupPromptResult {
		t.Logf("selecting prompt %d, second=%v", kind, second)
		done := make(chan setupPromptResult, 1)
		go func() {
			reply := make(chan setupPromptResult, 1)
			ui.prompts <- setupPromptRequest{kind: kind, reply: reply}
			done <- <-reply
		}()
		deadline := time.Now().Add(5 * time.Second)
		for time.Now().Before(deadline) {
			shown, _, _ := user32.NewProc("IsWindowVisible").Call(get(promptContinueID))
			if shown != 0 {
				break
			}
			time.Sleep(20 * time.Millisecond)
		}
		if second {
			procSendMessageW.Call(get(promptOption2ID), 0x00f5, 0, 0)
		}
		// Enter on the selected native option follows the default Continue action.
		procPostMessageW.Call(get(promptOption1ID), wmKeydown, 13, 0)
		select {
		case result := <-done:
			for deadline := time.Now().Add(time.Second); time.Now().Before(deadline); {
				shown, _, _ := user32.NewProc("IsWindowVisible").Call(get(promptContinueID))
				if shown == 0 {
					return result
				}
				time.Sleep(10 * time.Millisecond)
			}
			t.Fatal("prompt did not leave selection state")
			return result
		case <-time.After(5 * time.Second):
			t.Fatal("prompt did not finish from keyboard")
			return setupPromptResult{}
		}
	}
	if got := choose(setupPromptProvision, false); !got.primary {
		t.Fatal("personal account was not the default")
	}
	if got := choose(setupPromptProvision, true); got.primary {
		t.Fatal("quick start was not selected")
	}
	if got := choose(setupPromptSharedFolder, true); got.primary {
		t.Fatal("Not now did not decline sharing")
	}
	if got := choose(setupPromptShortcuts, true); !got.primary || !got.secondary {
		t.Fatalf("shortcut selections: %#v", got)
	}
}

func TestSetupPromptActionsDoNotOverlapOnSmallWorkArea(t *testing.T) {
	for _, height := range []int32{420, 404, 344, 324, 284} {
		title, body, first, second := setupPromptPositions(height)
		if title+24 > body || body+44 > first || first+28 > second || second+28 > height-50 {
			t.Fatalf("height %d overlaps: title=%d body=%d first=%d second=%d footer=%d", height, title, body, first, second, height-50)
		}
	}
}
