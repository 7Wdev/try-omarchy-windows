package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"regexp"
	"strconv"
	"strings"
	"time"
)

type usbDevice struct {
	Bus, Address, Vendor, Product, Class int
	Port, Name, ID                       string
	Connected, Claimed                   bool
}

type usbQMP interface {
	Call(context.Context, string, any, any) error
}
type usbBroker struct{ qmp usbQMP }

var usbHostRow = regexp.MustCompile(`(?m)^\s*Bus (\d+), Addr (\d+), Port ([0-9.]+), Speed [^\r\n]+\r?\n\s*Class ([0-9a-fA-F]{2}): USB device ([0-9a-fA-F]{4}):([0-9a-fA-F]{4})(?:, ([^\r\n]*))?`)
var usbPort = regexp.MustCompile(`^[0-9]+(?:\.[0-9]+){0,6}$`)

const usbControllerID = "tryomarchy-usb"
const usbDevicePrefix = "tryomarchy-usb-device-"

func (d usbDevice) identity() string {
	sum := sha256.Sum256([]byte(fmt.Sprintf("%d/%s/%04x/%04x", d.Bus, d.Port, d.Vendor, d.Product)))
	return usbDevicePrefix + hex.EncodeToString(sum[:12])
}
func (d usbDevice) validate() error {
	if d.Bus < 0 || d.Bus > 255 || d.Address < 1 || d.Address > 127 || d.Vendor < 0 || d.Vendor > 65535 || d.Product < 0 || d.Product > 65535 || !usbPort.MatchString(d.Port) || len(d.Port) > 27 {
		return fmt.Errorf("invalid USB device identity")
	}
	return nil
}
func parseUSBHostDevices(text string) ([]usbDevice, error) {
	if len(text) > 1<<20 {
		return nil, fmt.Errorf("USB inventory is too large")
	}
	var result []usbDevice
	for _, row := range usbHostRow.FindAllStringSubmatch(text, -1) {
		number := func(index, base int) int { value, _ := strconv.ParseInt(row[index], base, 32); return int(value) }
		d := usbDevice{Bus: number(1, 10), Address: number(2, 10), Port: row[3], Class: number(4, 16), Vendor: number(5, 16), Product: number(6, 16), Name: strings.TrimSpace(row[7]), Connected: true}
		if err := d.validate(); err != nil {
			return nil, err
		}
		if d.Name == "" {
			d.Name = fmt.Sprintf("USB %04x:%04x", d.Vendor, d.Product)
		}
		d.ID = d.identity()
		result = append(result, d)
	}
	if strings.TrimSpace(text) != "" && len(result) == 0 {
		return nil, uiError(uiTextWith("error.usb.list", map[string]string{"error": strings.TrimSpace(text)}), nil)
	}
	return result, nil
}
func (b usbBroker) hostDevices(ctx context.Context) ([]usbDevice, error) {
	var text string
	if err := b.qmp.Call(ctx, "human-monitor-command", map[string]any{"command-line": "info usbhost"}, &text); err != nil {
		return nil, err
	}
	return parseUSBHostDevices(text)
}

type usbQOMEntry struct {
	Name string `json:"name"`
	Type string `json:"type"`
}

func (b usbBroker) objects(ctx context.Context) ([]usbQOMEntry, error) {
	var objects []usbQOMEntry
	err := b.qmp.Call(ctx, "qom-list", map[string]any{"path": "/machine/peripheral"}, &objects)
	return objects, err
}
func (b usbBroker) Devices(ctx context.Context) ([]usbDevice, error) {
	devices, err := b.hostDevices(ctx)
	if err != nil {
		return nil, err
	}
	objects, err := b.objects(ctx)
	if err != nil {
		return nil, err
	}
	for _, object := range objects {
		if !strings.HasPrefix(object.Name, usbDevicePrefix) || object.Type != "child<usb-host>" {
			continue
		}
		d := usbDevice{ID: object.Name, Claimed: true}
		for _, property := range []struct {
			name   string
			target any
		}{{"hostbus", &d.Bus}, {"hostaddr", &d.Address}, {"vendorid", &d.Vendor}, {"productid", &d.Product}, {"hostport", &d.Port}, {"attached", &d.Connected}} {
			if err := b.qmp.Call(ctx, "qom-get", map[string]any{"path": "/machine/peripheral/" + object.Name, "property": property.name}, property.target); err != nil {
				return nil, err
			}
		}
		if d.ID != d.identity() {
			return nil, fmt.Errorf("USB attachment identity changed")
		}
		found := false
		for i := range devices {
			if devices[i].ID == d.ID {
				devices[i].Claimed = true
				devices[i].Connected = d.Connected && devices[i].Address == d.Address
				found = true
				break
			}
		}
		if !found {
			d.Connected = false
			d.Name = uiTextWith("usb.name.disconnected", map[string]string{"id": fmt.Sprintf("%04x:%04x", d.Vendor, d.Product)})
			devices = append(devices, d)
		}
	}
	return devices, nil
}
func (b usbBroker) Attach(ctx context.Context, selected usbDevice) error {
	if err := selected.validate(); err != nil {
		return err
	}
	devices, err := b.Devices(ctx)
	if err != nil {
		return err
	}
	found := false
	for _, current := range devices {
		if current.ID == selected.identity() && current.Address == selected.Address {
			if current.Claimed {
				return uiError(uiText("error.usb.attached"), nil)
			}
			found = true
		}
	}
	if !found {
		return uiError(uiText("error.usb.changed"), nil)
	}
	var properties []usbQOMEntry
	if err := b.qmp.Call(ctx, "device-list-properties", map[string]any{"typename": "usb-host"}, &properties); err != nil {
		return err
	}
	supported := false
	for _, property := range properties {
		if property.Name == "auto-reconnect" {
			supported = true
		}
	}
	if !supported {
		return uiError(uiText("error.usb.runtime"), nil)
	}
	objects, err := b.objects(ctx)
	if err != nil {
		return err
	}
	controller := false
	for _, object := range objects {
		if object.Name == usbControllerID {
			if object.Type != "child<qemu-xhci>" {
				return fmt.Errorf("USB controller identity is occupied")
			}
			controller = true
		}
	}
	if !controller {
		return uiError(uiText("error.usb.restart"), nil)
	}
	arguments := map[string]any{"driver": "usb-host", "id": selected.identity(), "bus": usbControllerID + ".0", "hostbus": selected.Bus, "hostaddr": selected.Address, "hostport": selected.Port, "vendorid": selected.Vendor, "productid": selected.Product, "auto-reconnect": false}
	if err := b.qmp.Call(ctx, "device_add", arguments, nil); err != nil {
		return uiError(uiTextWith("error.usb.release", map[string]string{"error": err.Error()}), err)
	}
	return nil
}
func (b usbBroker) Detach(ctx context.Context, id string) error {
	if !strings.HasPrefix(id, usbDevicePrefix) || len(id) != len(usbDevicePrefix)+24 {
		return fmt.Errorf("invalid USB attachment")
	}
	if _, err := hex.DecodeString(strings.TrimPrefix(id, usbDevicePrefix)); err != nil {
		return err
	}
	objects, err := b.objects(ctx)
	if err != nil {
		return err
	}
	found := false
	for _, object := range objects {
		if object.Name == id && object.Type == "child<usb-host>" {
			found = true
		}
	}
	if !found {
		return nil
	}
	if err := b.qmp.Call(ctx, "device_del", map[string]any{"id": id}, nil); err != nil {
		return err
	}
	for {
		objects, err = b.objects(ctx)
		if err != nil {
			return err
		}
		present := false
		for _, object := range objects {
			if object.Name == id {
				present = true
			}
		}
		if !present {
			return nil
		}
		select {
		case <-ctx.Done():
			return fmt.Errorf("USB release is still pending: %w", ctx.Err())
		case <-time.After(100 * time.Millisecond):
		}
	}
}
