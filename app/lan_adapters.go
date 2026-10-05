package main

import (
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

// Filter only for this launch. The saved addresses and adapter identities stay
// intact so a dock's rules return the next time its adapter is available.
func filterUnavailableForwardAdapters(forwards []portForward, bindings map[string]string, adapters []lanAdapter) (active, paused []portForward) {
	for _, forward := range forwards {
		identity := bindings[forward.String()]
		found := identity == ""
		for _, adapter := range adapters {
			if adapter.Identity != "" && strings.EqualFold(adapter.Identity, identity) {
				found = true
				break
			}
		}
		if found {
			active = append(active, forward)
		} else {
			paused = append(paused, forward)
		}
	}
	return active, paused
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
				return nil, uiError(uiTextWith("error.lan.adapter_missing", map[string]string{"forward": forward.String()}), nil)
			}
			forward.bind = address
		}
		if err := resolved.add(forward); err != nil {
			return nil, err
		}
	}
	return resolved, nil
}
