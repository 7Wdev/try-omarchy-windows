package main

import (
	"context"
	"crypto/ed25519"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

const stagedUpdateFilename = "staged-update"

func updatePayloadRoot(dir, portableRoot string, portable bool) string {
	if portable {
		return portableRoot
	}
	return filepath.Join(launcherUpdateDir(dir), "payloads")
}

func readStagedManifest(dir, version string, key ed25519.PublicKey) (*updateManifest, error) {
	if _, ok := parseReleaseVersion(version); !ok {
		return nil, fmt.Errorf("invalid staged version")
	}
	stage := filepath.Join(launcherUpdateDir(dir), version)
	read := func(name string, limit int64) ([]byte, error) {
		f, err := os.Open(filepath.Join(stage, name))
		if err != nil {
			return nil, err
		}
		defer f.Close()
		data, err := io.ReadAll(io.LimitReader(f, limit+1))
		if err != nil {
			return nil, err
		}
		if int64(len(data)) > limit {
			return nil, fmt.Errorf("staged metadata is too large")
		}
		return data, nil
	}
	data, err := read("update.json", maxUpdateManifestLen)
	if err != nil {
		return nil, err
	}
	sig, err := read("update.json.sig", maxUpdateSignatureLen)
	if err != nil {
		return nil, err
	}
	manifest, err := authenticateUpdateManifest(data, sig, key)
	if err != nil {
		return nil, err
	}
	if manifest.Version != version {
		return nil, fmt.Errorf("staged version does not match signature")
	}
	return manifest, nil
}

func stageSignedUpdate(ctx context.Context, client *http.Client, feed, dir, payloadRoot, installedVersion, installedDigest string, key ed25519.PublicKey) (*updateManifest, error) {
	data, err := fetchSmallFileContext(ctx, client, feed, maxUpdateManifestLen)
	if err != nil {
		return nil, err
	}
	sig, err := fetchSmallFileContext(ctx, client, feed+".sig", maxUpdateSignatureLen)
	if err != nil {
		return nil, err
	}
	manifest, err := authenticateUpdateManifest(data, sig, key)
	if err != nil {
		return nil, err
	}
	if !updateIsNewer(manifest.Version, installedVersion) &&
		(manifest.Version != installedVersion || manifest.ManifestSHA256 == installedDigest) {
		return nil, nil
	}
	if failedUpdateVersion(dir) == manifest.Version {
		return nil, nil
	}
	stage := filepath.Join(launcherUpdateDir(dir), manifest.Version)
	if err := validateMovePath(stage); err != nil {
		return nil, err
	}
	if err := os.MkdirAll(stage, 0700); err != nil {
		return nil, err
	}
	if err := stageUpdatePayload(ctx, payloadRoot, manifest.Release, manifest.ManifestSHA256, client, nil); err != nil {
		return nil, err
	}
	launcher := stagedLauncherPath(dir, manifest.Version)
	ok, err := verifyFileSHA256Context(ctx, launcher, manifest.Launcher.SHA256, nil)
	if err != nil {
		return nil, err
	}
	if !ok {
		logf("%s", uiText("status.downloading_launcher"))
		if err := removeUpdateFile(launcher); err != nil {
			return nil, err
		}
		size, err := updateDownloadSize(ctx, client, manifest.Release+"/"+manifest.Launcher.Name)
		if err != nil {
			return nil, err
		}
		if err := requireDiskSpace(stage, remainingFileBytes(launcher, size)+diskSpaceReserve); err != nil {
			return nil, err
		}
		opts := defaultDownloadOptions()
		opts.ctx = ctx
		if err := downloadVerifiedWithOptions(client, manifest.Release+"/"+manifest.Launcher.Name, launcher, manifest.Launcher.SHA256, nil, opts); err != nil {
			return nil, err
		}
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	for name, content := range map[string][]byte{"update.json": data, "update.json.sig": sig} {
		if err := writeUpdateFile(filepath.Join(stage, name), content); err != nil {
			return nil, err
		}
	}
	// This is the only ready marker. Partial downloads never become active state.
	if err := writeUpdateFile(filepath.Join(launcherUpdateDir(dir), stagedUpdateFilename), []byte(manifest.Version)); err != nil {
		return nil, err
	}
	return manifest, nil
}

func verifiedStagedUpdate(ctx context.Context, dir, payloadRoot, version string, key ed25519.PublicKey) (*updateManifest, error) {
	manifest, err := readStagedManifest(dir, version, key)
	if err != nil {
		return nil, err
	}
	if err := verifyUpdatePayload(ctx, filepath.Join(payloadRoot, manifest.ManifestSHA256), manifest.ManifestSHA256); err != nil {
		return nil, err
	}
	ok, err := verifyFileSHA256Context(ctx, stagedLauncherPath(dir, version), manifest.Launcher.SHA256, nil)
	if err != nil {
		return nil, err
	}
	if !ok {
		return nil, fmt.Errorf("staged launcher authentication failed")
	}
	return manifest, nil
}

func discardStagedUpdate(dir, payloadRoot, version string) {
	_ = removeUpdateFile(filepath.Join(launcherUpdateDir(dir), stagedUpdateFilename))
	if _, ok := parseReleaseVersion(version); !ok {
		return
	}
	// Read the signed identity separately when possible; never delete active or
	// rollback payloads just because a local pointer or signature is damaged.
	_ = os.RemoveAll(filepath.Join(launcherUpdateDir(dir), version))
}

func writeUpdateFile(path string, data []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	part := path + ".part"
	f, err := os.OpenFile(part, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	if _, err := f.Write(data); err != nil {
		f.Close()
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	return os.Rename(part, path)
}

const failedUpdateFilename = "failed-update"

func failedUpdateVersion(dir string) string {
	data, _ := os.ReadFile(filepath.Join(launcherUpdateDir(dir), failedUpdateFilename))
	return string(data)
}
func recordFailedUpdate(dir, version string) {
	if err := writeUpdateFile(filepath.Join(launcherUpdateDir(dir), failedUpdateFilename), []byte(version)); err != nil {
		logf("recording failed update: %v", err)
	}
}

// Bind every request (including redirects) to the guest lifetime. The body is
// paced to leave network capacity for the running guest; no total download timeout.
type backgroundUpdateTransport struct {
	ctx  context.Context
	base http.RoundTripper
}

func (t backgroundUpdateTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	ctx, cancel := context.WithCancel(req.Context())
	stop := context.AfterFunc(t.ctx, cancel)
	resp, err := t.base.RoundTrip(req.Clone(ctx))
	if err != nil {
		stop()
		cancel()
		return nil, err
	}
	resp.Body = &backgroundUpdateBody{ReadCloser: resp.Body, ctx: ctx, cancel: cancel, stop: stop}
	return resp, nil
}

type backgroundUpdateBody struct {
	io.ReadCloser
	ctx    context.Context
	cancel context.CancelFunc
	stop   func() bool
}

func (r *backgroundUpdateBody) Read(p []byte) (int, error) {
	if len(p) > 256<<10 {
		p = p[:256<<10]
	}
	n, err := r.ReadCloser.Read(p)
	if n > 0 {
		if e := sleepWithContext(r.ctx, time.Duration(n)*time.Second/(4<<20)); e != nil {
			return n, e
		}
	}
	return n, err
}
func (r *backgroundUpdateBody) Close() error { r.stop(); r.cancel(); return r.ReadCloser.Close() }

func (t backgroundUpdateTransport) CloseIdleConnections() {
	if c, ok := t.base.(interface{ CloseIdleConnections() }); ok {
		c.CloseIdleConnections()
	}
}
