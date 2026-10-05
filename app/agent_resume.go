package main

import "time"

type agentResumeTimer interface {
	Stop() bool
	Reset(time.Duration) bool
}

// The retry is coalesced without delaying periodic work or another wake.
func runAgentUpdates(timeTicks, batteryTicks <-chan time.Time, resumed, done <-chan struct{}, retryTicks <-chan time.Time, timer agentResumeTimer, sendTime func(string), sendBattery func()) {
	defer timer.Stop()
	for {
		select {
		case <-done:
			return
		case <-timeTicks:
			sendTime("")
		case <-batteryTicks:
			sendBattery()
		case <-resumed:
			sendTime("resume")
			sendBattery()
			if !timer.Stop() {
				select {
				case <-retryTicks:
				default:
				}
			}
			timer.Reset(5 * time.Second)
		case <-retryTicks:
			sendTime("resume")
			sendBattery()
		}
	}
}
