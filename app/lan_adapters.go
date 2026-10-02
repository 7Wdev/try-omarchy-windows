package main

import (
	"fmt"
	"sort"
	"strings"
)

type lanAdapter struct{ Name, Address, Identity string }

func availableLANAdapters() ([]lanAdapter, error) {
	result, err := platformLANAdapters()
	if err != nil {
		return nil, err
	}
	sort.Slice(result, func(i, j int) bool {
		if result[i].Name == result[j].Name {
			return result[i].Address < result[j].Address
		}
		return result[i].Name < result[j].Name
	})
	result = append(result, lanAdapter{Name: uiText("lan.all_adapters"), Address: "0.0.0.0"})
	return result, nil
}

func resolveForwardAdapters(forwards []portForward, bindings map[string]string, adapters []lanAdapter) ([]portForward, error) {
	var resolved forwardList
	for _, forward := range forwards {
		if identity := bindings[forward.String()]; identity != "" {
			found := false
			address := ""
			for _, adapter := range adapters {
				if strings.EqualFold(adapter.Identity, identity) {
					if !found || adapter.Address == forward.bind {
						address = adapter.Address
					}
					found = true
					if adapter.Address == forward.bind {
						break
					}
				}
			}
			if !found {
				return nil, fmt.Errorf("the network adapter for %s is unavailable; remove or update this forward in Settings, then select an available adapter with Add LAN", forward.String())
			}
			forward.bind = address
		}
		if err := resolved.add(forward); err != nil {
			return nil, err
		}
	}
	return resolved, nil
}
