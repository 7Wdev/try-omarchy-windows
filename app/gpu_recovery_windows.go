//go:build windows

package main

// The VM stays alive while the user chooses. Dismissing this dialog preserves
// the session, including a slow compositor that recovers while it is open.
func chooseFreezeRecovery(cancel <-chan struct{}, gpu bool, trigger, bundle string, snapshotErr error, suggest bool) gpuRecoveryDecision {
	body := uiText("recovery.display.body")
	if trigger == "compositor" {
		body = uiText("recovery.compositor.body")
	}
	if gpu {
		body += "\n\n" + uiText("recovery.gpu.body")
	}
	if suggest {
		body += "\n\n" + uiText("recovery.gpu.suggest")
	}
	if snapshotErr == nil {
		body += "\n\n" + uiTextWith("recovery.gpu.saved", map[string]string{"path": bundle})
	} else {
		body += "\n\n" + uiTextWith("recovery.gpu.save_failed", map[string]string{"error": snapshotErr.Error()})
	}
	var action int
	var err error
	if gpu {
		action, err = chooseActionCancelable(cancel, uiText("recovery.gpu.title"), body, 260,
			uiText("recovery.gpu.cpu_once"), uiText("recovery.gpu.gpu"), uiText("recovery.gpu.close"), uiText("recovery.gpu.cpu_always"))
	} else {
		action, err = chooseActionCancelable(cancel, uiText("recovery.gpu.title"), body, 260, uiText("recovery.cpu.restart"), uiText("recovery.gpu.close"))
		if action != 1 {
			action = gpuRecoveryClose
		}
	}
	if err != nil {
		logf("display freeze recovery dialog failed: %v", err)
		return gpuRecoveryDecision{}
	}
	decision := decideGPURecovery(action)
	if !gpu {
		decision.temporaryCPU = false
	}
	return decision
}
