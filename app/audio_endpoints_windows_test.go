//go:build windows

package main

import (
	"errors"
	"reflect"
	"testing"
)

func TestMMAudioEndpointsSkipUnreadableDevice(t *testing.T) {
	want := []audioEndpointInfo{{ID: "first", Name: "Speakers", SampleRate: 48000}, {ID: "last", Name: "USB", SampleRate: 44100}}
	var visited []uint32
	got := collectMMAudioEndpoints(3, func(i uint32) (audioEndpointInfo, error) {
		visited = append(visited, i)
		if i == 1 {
			return audioEndpointInfo{}, errors.New("reading audio endpoint name failed")
		}
		return want[i/2], nil
	})
	if !reflect.DeepEqual(got, want) || !reflect.DeepEqual(visited, []uint32{0, 1, 2}) {
		t.Fatalf("endpoints=%+v visited=%v", got, visited)
	}
}
