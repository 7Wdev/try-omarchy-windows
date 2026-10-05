//go:build windows

package main

import (
	"bytes"
	"os"
	"runtime"
	"strings"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

// MFEnumDeviceSources is an mf.dll export, not the mfplat.dll one the capture
// code first assumed, and Windows builds have moved these entry points between
// mfplat.dll, mf.dll and mfcore.dll. Every one must resolve on a Windows with
// Media Foundation, and a missing export must surface as an error instead of
// the panic LazyProc.Call raises.
func TestMediaFoundationExportsResolve(t *testing.T) {
	api, err := mediaFoundation()
	if err != nil {
		t.Fatalf("resolving Media Foundation entry points: %v", err)
	}
	for name, proc := range map[string]*syscall.LazyProc{
		"MFStartup":                           api.startup,
		"MFCreateAttributes":                  api.createAttributes,
		"MFCreateMediaType":                   api.createMediaType,
		"MFEnumDeviceSources":                 api.enumDeviceSources,
		"MFCreateSourceReaderFromMediaSource": api.createSourceReader,
	} {
		if proc == nil {
			t.Fatalf("%s was not resolved", name)
		}
	}
}

func TestFindMFProcReportsMissingExports(t *testing.T) {
	if _, err := findMFProc("MFThisExportDoesNotExist"); err == nil {
		t.Fatal("a missing Media Foundation export resolved without an error")
	}
}

// Exercise the real IMFSample ABI without requiring a camera on the CI runner.
// A made-up vtable would repeat the implementation's original indexing error.
func TestCameraCopiesNativeMediaSample(t *testing.T) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if hr := procCall(procCoInitializeEx, 0, coInitMultithreaded); hr < 0 {
		t.Fatalf("COM initialization: %#x", uint32(hr))
	}
	defer procCoUninitialize.Call()
	if err := startMediaFoundation(); err != nil {
		t.Fatal(err)
	}
	createSample, err := findMFProc("MFCreateSample")
	if err != nil {
		t.Fatal(err)
	}
	createBuffer, err := findMFProc("MFCreateMemoryBuffer")
	if err != nil {
		t.Fatal(err)
	}
	var sample, buffer unsafe.Pointer
	if hr := procCall(createSample, uintptr(unsafe.Pointer(&sample))); hr < 0 {
		t.Fatalf("create sample: %#x", uint32(hr))
	}
	defer mfRelease(&sample)
	stride := cameraWidth + 32
	length := stride * cameraHeight * 3 / 2
	if hr := procCall(createBuffer, uintptr(length), uintptr(unsafe.Pointer(&buffer))); hr < 0 {
		t.Fatalf("create buffer: %#x", uint32(hr))
	}
	defer mfRelease(&buffer)
	var data unsafe.Pointer
	var capacity, used uint32
	if hr := mfCall(buffer, 3, uintptr(unsafe.Pointer(&data)), uintptr(unsafe.Pointer(&capacity)), uintptr(unsafe.Pointer(&used))); hr < 0 {
		t.Fatalf("lock buffer: %#x", uint32(hr))
	}
	rows := unsafe.Slice((*byte)(data), length)
	for i := range rows {
		rows[i] = 0xee
	}
	for row := 0; row < cameraHeight*3/2; row++ {
		for x := 0; x < cameraWidth; x++ {
			rows[row*stride+x] = byte(row)
		}
	}
	mfCall(buffer, 4)                                     // Unlock
	if hr := mfCall(buffer, 6, uintptr(length)); hr < 0 { // SetCurrentLength
		t.Fatalf("set length: %#x", uint32(hr))
	}
	if hr := mfCall(sample, 42, uintptr(buffer)); hr < 0 { // AddBuffer
		t.Fatalf("add buffer: %#x", uint32(hr))
	}
	for _, invalid := range []int{-cameraWidth, 0, 1 << 30} {
		if frame := (&mfCameraSource{stride: invalid}).copyFrame(uintptr(sample)); frame != nil {
			t.Fatalf("unsafe stride %d copied a frame", invalid)
		}
	}
	source := &mfCameraSource{stride: stride}
	frame := source.copyFrame(uintptr(sample))
	if len(frame) != cameraFrameBytes {
		t.Fatalf("frame length: %d", len(frame))
	}
	for row := 0; row < cameraHeight*3/2; row++ {
		if !bytes.Equal(frame[row*cameraWidth:(row+1)*cameraWidth], bytes.Repeat([]byte{byte(row)}, cameraWidth)) {
			t.Fatalf("incorrect pixels/padding in row %d", row)
		}
	}
}

func TestCameraCallbackInterfaceAndLifetime(t *testing.T) {
	callback := newCameraCallback(&mfCameraSource{})
	this := uintptr(unsafe.Pointer(callback))
	var result uintptr
	unknown := newGUID(123, 0, 0)
	if hr := callbackQueryInterface(this, uintptr(unsafe.Pointer(&unknown)), uintptr(unsafe.Pointer(&result))); hr != 0x80004002 || result != 0 {
		t.Fatalf("unsupported interface: hr=%#x result=%#x", hr, result)
	}
	if hr := callbackQueryInterface(this, uintptr(unsafe.Pointer(&guidSourceReaderCallback)), uintptr(unsafe.Pointer(&result))); hr != 0 || result != this {
		t.Fatalf("callback interface: hr=%#x result=%#x", hr, result)
	}
	if refs := callbackRelease(result); refs != 1 {
		t.Fatalf("references: %d", refs)
	}
	runtime.GC()
	if _, ok := cameraCallbacks.Load(callback); !ok {
		t.Fatal("live callback was not retained")
	}
	if refs := callbackRelease(this); refs != 0 {
		t.Fatalf("references: %d", refs)
	}
	if _, ok := cameraCallbacks.Load(callback); ok {
		t.Fatal("released callback leaked")
	}
}

func TestNativeCameraCaptureRestart(t *testing.T) {
	if os.Getenv("TRYOMARCHY_TEST_CAMERA") != "1" {
		t.Skip("requires a real camera and user opt-in")
	}
	source := &mfCameraSource{}
	defer source.stop()
	for cycle := 0; cycle < 3; cycle++ {
		frames, err := source.start()
		if err != nil {
			t.Fatal(err)
		}
		runtime.GC()
		select {
		case frame, ok := <-frames:
			if !ok || len(frame) != cameraFrameBytes {
				t.Fatalf("cycle %d: invalid frame", cycle)
			}
		case <-time.After(15 * time.Second):
			t.Fatalf("cycle %d: no camera frame", cycle)
		}
		source.stop()
	}
}

func TestSelectedMissingCameraDoesNotFallBack(t *testing.T) {
	if os.Getenv("TRYOMARCHY_TEST_CAMERA") != "1" {
		t.Skip("requires a camera-enabled Windows test host")
	}
	source := &mfCameraSource{deviceID: "try-omarchy-intentionally-missing-device"}
	defer source.stop()
	if _, err := source.start(); err == nil || !strings.Contains(err.Error(), "selected camera is disconnected") {
		t.Fatalf("missing selected camera silently fell back: %v", err)
	}
}

func TestCameraNV12MetadataBounds(t *testing.T) {
	for _, stride := range []int64{-cameraWidth, 0, cameraWidth - 1, 1 << 31, (1<<31-1)/(cameraHeight*3/2) + 1} {
		if length, ok := cameraNV12Length(stride); ok {
			t.Fatalf("unsafe stride %d accepted with length %d", stride, length)
		}
	}
	for _, stride := range []int64{cameraWidth, cameraWidth + 32} {
		length, ok := cameraNV12Length(stride)
		if !ok || int64(length) != stride*(cameraHeight*3/2) {
			t.Fatalf("valid stride %d: length=%d ok=%v", stride, length, ok)
		}
	}
}

func TestCameraCallbacksReuseAddressesAcrossStarts(t *testing.T) {
	first := newCameraCallback(&mfCameraSource{})
	want := *first.vtable
	callbackRelease(uintptr(unsafe.Pointer(first)))
	for cycle := 0; cycle < 400; cycle++ {
		callback := newCameraCallback(&mfCameraSource{})
		if *callback.vtable != want {
			t.Fatal("callback trampolines were not reused")
		}
		callbackRelease(uintptr(unsafe.Pointer(callback)))
		if _, retained := cameraCallbacks.Load(callback); retained {
			t.Fatal("released callback remains retained")
		}
	}
}

func TestCameraMediaTypeChanges(t *testing.T) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if hr := procCall(procCoInitializeEx, 0, coInitMultithreaded); hr < 0 {
		t.Fatalf("COM init: %#x", uint32(hr))
	}
	defer procCoUninitialize.Call()
	if err := startMediaFoundation(); err != nil {
		t.Fatal(err)
	}
	api, err := mediaFoundation()
	if err != nil {
		t.Fatal(err)
	}
	var media unsafe.Pointer
	if hr := procCall(api.createMediaType, uintptr(unsafe.Pointer(&media))); hr < 0 {
		t.Fatalf("media type: %#x", uint32(hr))
	}
	defer mfRelease(&media)
	var reads int
	var vtable [10]uintptr
	vtable[6] = syscall.NewCallback(func(this, stream, out uintptr) uintptr {
		mfCall(media, 1) // GetCurrentMediaType transfers a reference.
		*(*unsafe.Pointer)(unsafe.Pointer(out)) = media
		return 0
	})
	vtable[9] = syscall.NewCallback(func(this, stream, flags, index, resultFlags, timestamp, sample uintptr) uintptr {
		reads++
		return 0
	})
	reader := struct{ vtable *[10]uintptr }{&vtable}
	for _, tc := range []struct {
		name    string
		flags   uint32
		subtype comGUID
		size    uint64
		stride  uint32
		ended   bool
	}{
		{"supported change", 32, guidVideoFormatNV12, packUint32Pair(cameraWidth, cameraHeight), cameraWidth + 32, false},
		{"wrong subtype", 32, guidMediaTypeVideo, packUint32Pair(cameraWidth, cameraHeight), cameraWidth, true},
		{"wrong size", 32, guidVideoFormatNV12, packUint32Pair(cameraWidth+2, cameraHeight), cameraWidth + 2, true},
		{"negative stride", 32, guidVideoFormatNV12, packUint32Pair(cameraWidth, cameraHeight), 0xfffffd80, true},
		{"huge stride", 32, guidVideoFormatNV12, packUint32Pair(cameraWidth, cameraHeight), 0x7fffffff, true},
		{"EOS wins over change", 34, guidVideoFormatNV12, packUint32Pair(cameraWidth, cameraHeight), cameraWidth, true},
		{"error wins over change", 33, guidVideoFormatNV12, packUint32Pair(cameraWidth, cameraHeight), cameraWidth, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			setGUID(media, &guidSubtype, &tc.subtype)
			setUint64(media, &guidFrameSize, tc.size)
			setUint32(media, &guidDefaultStride, tc.stride)
			source := &mfCameraSource{reader: unsafe.Pointer(&reader), frames: make(chan []byte, 1)}
			callback := &cameraCallback{source: source}
			source.callback = callback
			reads = 0
			source.handleSample(callback, sOK, tc.flags, 0)
			if source.ended != tc.ended || (reads == 1) == tc.ended {
				t.Fatalf("ended=%v reads=%d", source.ended, reads)
			}
			if !tc.ended && source.stride != int(tc.stride) {
				t.Fatalf("stride=%d", source.stride)
			}
		})
	}
	runtime.KeepAlive(reader)
	runtime.KeepAlive(vtable)
}
