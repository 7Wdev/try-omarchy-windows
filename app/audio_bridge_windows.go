//go:build windows

package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"time"
)

// The guest chooses a stable Core Audio endpoint ID. The host alone turns
// that ID into an SDL name and writes QEMU's private live-route control file.
// This avoids exposing any host command execution through the virtio port.
type audioBridgeDevice struct {
	UID  string `json:"deviceUID"`
	Name string `json:"name"`
}

type audioBridgeCatalog struct {
	Type              string              `json:"type"`
	VolumeSync        bool                `json:"volumeSync,omitempty"`
	Outputs           []audioBridgeDevice `json:"outputs"`
	Inputs            []audioBridgeDevice `json:"inputs"`
	SelectedOutputUID *string             `json:"selectedOutputUID"`
	SelectedInputUID  *string             `json:"selectedInputUID"`
}

type audioBridgeRequest struct {
	Type       string   `json:"type"`
	Direction  string   `json:"direction,omitempty"`
	UID        *string  `json:"deviceUID"`
	EndpointID string   `json:"endpointID,omitempty"`
	Volume     *float64 `json:"volume,omitempty"`
	Muted      *bool    `json:"muted,omitempty"`
	Origin     string   `json:"origin,omitempty"`
	Sequence   uint64   `json:"sequence,omitempty"`
}

func audioBridgeDevices(endpoints []audioEndpointInfo, sdlNames []string) []audioBridgeDevice {
	result := make([]audioBridgeDevice, 0, len(endpoints))
	sdlCount := make(map[string]int, len(sdlNames))
	endpointCount := make(map[string]int, len(endpoints))
	for _, name := range sdlNames {
		sdlCount[name]++
	}
	for _, endpoint := range endpoints {
		endpointCount[endpoint.Name]++
	}
	for _, endpoint := range endpoints {
		// SDL routes by name. Never offer an ambiguous or missing name as a
		// stable-ID choice that might silently select the wrong physical device.
		if endpoint.ID != "" && endpoint.Name != "" &&
			sdlCount[endpoint.Name] == 1 && endpointCount[endpoint.Name] == 1 {
			result = append(result, audioBridgeDevice{UID: endpoint.ID, Name: endpoint.Name})
		}
	}
	return result
}

func audioBridgeSelected(id, name string, devices []audioBridgeDevice) *string {
	for _, device := range devices {
		if device.UID == id && id != "" {
			selected := id
			return &selected
		}
	}
	if id == "" && name != "" {
		for _, device := range devices {
			if device.Name == name {
				selected := device.UID
				return &selected
			}
		}
	}
	return nil
}

func currentAudioBridgeCatalog(dataDir, qemu string, microphoneDisabledAtBoot bool) (audioBridgeCatalog, error) {
	catalog := audioBridgeCatalog{Type: "catalog", VolumeSync: true, Outputs: []audioBridgeDevice{}, Inputs: []audioBridgeDevice{}}
	names, err := loadAudioPreferences(dataDir)
	if err != nil {
		return catalog, err
	}
	ids, err := loadAudioEndpoints(dataDir)
	if err != nil {
		return catalog, err
	}
	desktop, err := loadDesktopPreferences(dataDir)
	if err != nil {
		return catalog, err
	}
	endpoints, err := listAudioEndpoints()
	if err != nil {
		return catalog, err
	}
	sdl, err := listAudioDevices(qemu)
	if err != nil {
		return catalog, err
	}
	catalog.Outputs = audioBridgeDevices(endpoints.Output, sdl.Output)
	catalog.SelectedOutputUID = audioBridgeSelected(ids.OutputID, names.Output, catalog.Outputs)
	if !desktop.MicrophoneDisabled && !microphoneDisabledAtBoot {
		catalog.Inputs = audioBridgeDevices(endpoints.Input, sdl.Input)
		catalog.SelectedInputUID = audioBridgeSelected(ids.InputID, names.Input, catalog.Inputs)
	}
	return catalog, nil
}

func parseAudioBridgeRequest(line []byte) (audioBridgeRequest, error) {
	var request audioBridgeRequest
	decoder := json.NewDecoder(bytes.NewReader(line))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&request); err != nil {
		return request, err
	}
	if decoder.Decode(&struct{}{}) != io.EOF {
		return request, fmt.Errorf("trailing audio bridge data")
	}
	switch request.Type {
	case "get-volume":
		if request.Direction != "" || request.UID != nil || request.EndpointID != "" || request.Volume != nil || request.Muted != nil || request.Origin != "" || request.Sequence != 0 {
			return request, fmt.Errorf("invalid volume request")
		}
	case "set-volume":
		if request.Direction != "" || request.UID != nil || request.Volume == nil || request.Muted == nil || request.Origin != "guest" || request.Sequence == 0 || !(endpointVolume{request.EndpointID, *request.Volume, *request.Muted}).valid() {
			return request, fmt.Errorf("invalid volume state")
		}
	case "get-catalog":
		if request.Direction != "" || request.UID != nil {
			return request, fmt.Errorf("invalid catalog request")
		}
	case "select":
		if request.Direction != "output" && request.Direction != "input" {
			return request, fmt.Errorf("invalid audio direction")
		}
		if request.UID != nil && *request.UID == "" {
			return request, fmt.Errorf("empty audio endpoint ID")
		}
	default:
		return request, fmt.Errorf("unknown audio bridge request")
	}
	if request.Type == "get-catalog" || request.Type == "select" {
		if request.EndpointID != "" || request.Volume != nil || request.Muted != nil || request.Origin != "" || request.Sequence != 0 {
			return request, fmt.Errorf("volume fields in route request")
		}
	}
	return request, nil
}

func applyAudioBridgeSelection(dataDir string, catalog audioBridgeCatalog, request audioBridgeRequest, microphoneDisabledAtBoot bool) error {
	devices := catalog.Outputs
	if request.Direction == "input" {
		devices = catalog.Inputs
	}
	name := ""
	id := ""
	if request.UID != nil {
		found := false
		for _, device := range devices {
			if device.UID == *request.UID {
				name, id, found = device.Name, device.UID, true
				break
			}
		}
		if !found {
			return fmt.Errorf("audio endpoint is no longer available")
		}
	}
	names, err := loadAudioPreferences(dataDir)
	if err != nil {
		return err
	}
	ids, err := loadAudioEndpoints(dataDir)
	if err != nil {
		return err
	}
	desktop, err := loadDesktopPreferences(dataDir)
	if err != nil {
		return err
	}
	if request.Direction == "input" {
		if desktop.MicrophoneDisabled || microphoneDisabledAtBoot {
			return fmt.Errorf("microphone access is disabled")
		}
		names.Input, ids.InputID = name, id
	} else {
		names.Output, ids.OutputID = name, id
	}
	if err := saveAudioSelection(dataDir, names, ids); err != nil {
		return err
	}
	return publishSavedAudioRoutes(dataDir, names, desktop.MicrophoneDisabled)
}

func writeAudioBridgeCatalog(conn net.Conn, catalog any) ([]byte, error) {
	message, err := json.Marshal(catalog)
	if err != nil {
		return nil, err
	}
	if len(message) > 65535 {
		return nil, fmt.Errorf("audio catalog is too large")
	}
	if err := conn.SetWriteDeadline(time.Now().Add(5 * time.Second)); err != nil {
		return nil, err
	}
	payload := append(message, '\n')
	n, err := conn.Write(payload)
	if err == nil && n != len(payload) {
		err = io.ErrShortWrite
	}
	return message, err
}

func serveAudioBridge(conn net.Conn, dataDir, qemu string, microphoneDisabledAtBoot bool) error {
	defer conn.Close()
	lines := make(chan []byte, 1)
	readErr := make(chan error, 1)
	stop := make(chan struct{})
	defer close(stop)
	go func() {
		scanner := bufio.NewScanner(conn)
		scanner.Buffer(make([]byte, 4096), 8192)
		for scanner.Scan() {
			select {
			case lines <- append([]byte(nil), scanner.Bytes()...):
			case <-stop:
				return
			}
		}
		select {
		case readErr <- scanner.Err():
		case <-stop:
		}
	}()
	ticker := time.NewTicker(3 * time.Second)
	defer ticker.Stop()
	var previous []byte
	refresh := func(force bool) error {
		catalog, err := currentAudioBridgeCatalog(dataDir, qemu, microphoneDisabledAtBoot)
		if err != nil {
			return err
		}
		encoded, err := json.Marshal(catalog)
		if err != nil {
			return err
		}
		if force || !bytes.Equal(previous, encoded) {
			previous, err = writeAudioBridgeCatalog(conn, catalog)
		}
		return err
	}
	if err := refresh(true); err != nil {
		return err
	}
	var volume *windowsVolumeEndpoint
	var volumeChanges <-chan endpointVolume
	var sync volumeSync
	var debounce *time.Timer
	var due <-chan time.Time
	defer func() {
		if debounce != nil {
			debounce.Stop()
		}
		if volume != nil {
			volume.close()
		}
	}()
	publishVolume := func() error {
		_, err := writeAudioBridgeCatalog(conn, sync.message)
		return err
	}
	volumeEnabled := func() bool {
		prefs, err := loadDesktopPreferences(dataDir)
		return err == nil && !prefs.VolumeSyncDisabled && volume != nil
	}
	startVolume := func() {
		var initial endpointVolume
		var err error
		volume, initial, err = startWindowsVolumeEndpoint()
		if err != nil {
			logf("audio bridge: volume: %v", err)
			initial = endpointVolume{}
		}
		if volume != nil {
			volumeChanges = volume.changes
		}
		sync.observe(initial, volumeEnabled(), "windows")
	}
	for {
		select {
		case line := <-lines:
			request, err := parseAudioBridgeRequest(line)
			if err != nil {
				return err
			}
			if request.Type == "get-volume" {
				if sync.message.Sequence == 0 {
					startVolume()
					// Only guests with volume sync ask, so this line in a
					// diagnostics bundle tells an old guest from an off switch.
					logf("audio bridge: guest volume sync handshake, sync active=%v", sync.message.Enabled)
				}
				if err := publishVolume(); err != nil {
					return err
				}
				continue
			}
			if request.Type == "set-volume" {
				if sync.message.Sequence == 0 {
					return fmt.Errorf("volume handshake required")
				}
				if sync.request(volumeSyncRequest{request.Origin, request.Sequence, endpointVolume{request.EndpointID, *request.Volume, *request.Muted}}) {
					if debounce == nil {
						debounce = time.NewTimer(volumeSyncDebounce)
					} else {
						if !debounce.Stop() {
							select {
							case <-debounce.C:
							default:
							}
						}
						debounce.Reset(volumeSyncDebounce)
					}
					due = debounce.C
				} else if err := publishVolume(); err != nil {
					return err
				}
				continue
			}
			if request.Type == "select" {
				catalog, err := currentAudioBridgeCatalog(dataDir, qemu, microphoneDisabledAtBoot)
				if err != nil {
					return err
				}
				if err := applyAudioBridgeSelection(dataDir, catalog, request, microphoneDisabledAtBoot); err != nil {
					logf("audio bridge: selection ignored: %v", err)
				}
			}
			if err := refresh(true); err != nil {
				return err
			}
		case <-ticker.C:
			if err := refresh(false); err != nil {
				logf("audio bridge: catalog refresh: %v", err)
			}
			if sync.message.Sequence != 0 {
				if volume == nil {
					startVolume()
				} else {
					sync.observe(sync.message.endpointVolume, volumeEnabled(), "windows")
				}
				// Heartbeats keep an idle guest from retaining bypassed controls
				// after the launcher or its bridge disappears.
				if err := publishVolume(); err != nil {
					return err
				}
			}
		case value := <-volumeChanges:
			if sync.observe(value, volumeEnabled(), "windows") {
				if err := publishVolume(); err != nil {
					return err
				}
			}
		case <-due:
			due = nil
			if !volumeEnabled() {
				sync.observe(sync.message.endpointVolume, false, "windows")
			}
			if value, apply := sync.take(); apply && volume != nil {
				actual, err := volume.set(value, sync.message.endpointVolume)
				if err != nil {
					logf("audio bridge: volume: %v", err)
				}
				origin := "guest"
				if err != nil || !actual.equal(value) {
					origin = "windows"
				}
				sync.acknowledge(actual, volumeEnabled(), origin)
			}
			if err := publishVolume(); err != nil {
				return err
			}
		case err := <-readErr:
			if err == nil || errors.Is(err, io.EOF) {
				return nil
			}
			return err
		}
	}
}

func runAudioBridge(dataDir, qemu string, microphoneDisabledAtBoot bool) {
	listener, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", audioBridgePort))
	if err != nil {
		fatal(uiTextWith("fatal.port.audio", map[string]string{"port": fmt.Sprint(audioBridgePort)}))
	}
	logf("audio: bridge listening on %d", audioBridgePort)
	// Only this launcher's QEMU may read or change audio devices. Each
	// connection polls the device catalog, so the slots stay few: QEMU's own
	// and one for a reconnect while the old connection closes.
	go acceptQEMUBridge(listener, "audio bridge", 2, func(conn net.Conn) {
		if err := serveAudioBridge(conn, dataDir, qemu, microphoneDisabledAtBoot); err != nil {
			logf("audio bridge: %v", err)
		}
	})
}
