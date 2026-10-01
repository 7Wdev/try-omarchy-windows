package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"time"
)

const maxQMPMessage = 2 << 20

// Feature commands use their own connection, separate from the supervisor's
// event stream. Only one command is outstanding on this connection at a time.
type qmpClient struct {
	conn    net.Conn
	lines   *bufio.Scanner
	gate    chan struct{}
	nextID  uint64
	broken  bool
	onEvent func(string)
}

type qmpCommandError struct {
	Class       string `json:"class"`
	Description string `json:"desc"`
}

func (e *qmpCommandError) Error() string { return e.Class + ": " + e.Description }

type qmpMessage struct {
	Greeting json.RawMessage  `json:"QMP"`
	Return   json.RawMessage  `json:"return"`
	Error    *qmpCommandError `json:"error"`
	Event    string           `json:"event"`
	ID       json.RawMessage  `json:"id"`
}

func dialQMPClient(ctx context.Context, address string) (*qmpClient, error) {
	conn, err := (&net.Dialer{}).DialContext(ctx, "tcp", address)
	if err != nil {
		return nil, err
	}
	return newQMPClient(ctx, conn)
}

// newQMPClient owns conn even when negotiation fails.
func newQMPClient(ctx context.Context, conn net.Conn) (*qmpClient, error) {
	c := &qmpClient{conn: conn, lines: bufio.NewScanner(conn), gate: make(chan struct{}, 1)}
	c.lines.Buffer(make([]byte, 4096), maxQMPMessage)
	cleanup, err := c.deadline(ctx)
	if err != nil {
		conn.Close()
		return nil, err
	}
	greeting, err := c.read()
	cleanup()
	if err != nil {
		conn.Close()
		return nil, err
	}
	var version struct {
		Version      json.RawMessage `json:"version"`
		Capabilities []string        `json:"capabilities"`
	}
	if len(greeting.Greeting) == 0 || json.Unmarshal(greeting.Greeting, &version) != nil || len(version.Version) == 0 || string(version.Version) == "null" {
		conn.Close()
		return nil, fmt.Errorf("invalid QMP greeting")
	}
	if err := c.Call(ctx, "qmp_capabilities", nil, nil); err != nil {
		conn.Close()
		return nil, err
	}
	return c, nil
}

func (c *qmpClient) Close() error { return c.conn.Close() }

// Cancellation closes this connection. A timed-out command may have executed;
// callers must inspect runtime state before deciding whether to retry it.
func (c *qmpClient) deadline(ctx context.Context) (func(), error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	end := time.Now().Add(8 * time.Second)
	if deadline, ok := ctx.Deadline(); ok && deadline.Before(end) {
		end = deadline
	}
	if err := c.conn.SetDeadline(end); err != nil {
		return nil, err
	}
	stop := context.AfterFunc(ctx, func() { c.conn.Close() })
	return func() { stop(); c.conn.SetDeadline(time.Time{}) }, nil
}

func (c *qmpClient) read() (qmpMessage, error) {
	var message qmpMessage
	if !c.lines.Scan() {
		if err := c.lines.Err(); err != nil {
			return message, err
		}
		return message, fmt.Errorf("QMP connection closed before a reply")
	}
	if err := json.Unmarshal(c.lines.Bytes(), &message); err != nil {
		return message, fmt.Errorf("invalid QMP message: %w", err)
	}
	kinds := 0
	for _, present := range []bool{len(message.Greeting) > 0, len(message.Return) > 0, message.Error != nil, message.Event != ""} {
		if present {
			kinds++
		}
	}
	if kinds != 1 {
		return message, fmt.Errorf("ambiguous QMP message")
	}
	return message, nil
}

func (c *qmpClient) Call(ctx context.Context, command string, arguments any, result any) (err error) {
	select {
	case c.gate <- struct{}{}:
	case <-ctx.Done():
		return ctx.Err()
	}
	defer func() { <-c.gate }()
	if c.broken {
		return fmt.Errorf("QMP connection is unusable after an interrupted command")
	}
	if command == "" {
		return fmt.Errorf("missing QMP command")
	}
	c.nextID++
	id := fmt.Sprintf("try-omarchy-%d", c.nextID)
	request := struct {
		Execute   string `json:"execute"`
		Arguments any    `json:"arguments,omitempty"`
		ID        string `json:"id"`
	}{command, arguments, id}
	data, err := json.Marshal(request)
	if err != nil {
		return err
	}
	if len(data) >= maxQMPMessage {
		return fmt.Errorf("QMP command is too large")
	}
	cleanup, err := c.deadline(ctx)
	if err != nil {
		return err
	}
	defer cleanup()
	defer func() {
		if ctx.Err() != nil {
			err = ctx.Err()
		}
		var remote *qmpCommandError
		if err != nil && !errors.As(err, &remote) {
			c.broken = true
			c.conn.Close()
		}
	}()
	if _, err = c.conn.Write(append(data, '\n')); err != nil {
		return err
	}
	for {
		message, readErr := c.read()
		if readErr != nil {
			return readErr
		}
		if message.Event != "" {
			if c.onEvent != nil {
				c.onEvent(message.Event)
			}
			continue
		}
		var responseID string
		if json.Unmarshal(message.ID, &responseID) != nil || responseID != id {
			return fmt.Errorf("QMP reply does not match %s", command)
		}
		if message.Error != nil {
			return message.Error
		}
		if len(message.Return) == 0 {
			return fmt.Errorf("missing QMP result for %s", command)
		}
		if result != nil {
			if err = json.Unmarshal(message.Return, result); err != nil {
				return fmt.Errorf("invalid QMP result for %s: %w", command, err)
			}
		}
		return nil
	}
}

type vmRuntimeStatus struct {
	Running bool   `json:"running"`
	Status  string `json:"status"`
}

type vmRuntimeCapabilities struct {
	State             vmRuntimeStatus `json:"state"`
	Commands          map[string]bool `json:"commands"`
	MigrationBlockers []string        `json:"migrationBlockers"`
}

// Command presence alone does not establish device-state migration support.
func inspectVMRuntime(ctx context.Context, c *qmpClient) (vmRuntimeCapabilities, error) {
	r := vmRuntimeCapabilities{Commands: map[string]bool{}}
	if err := c.Call(ctx, "query-status", nil, &r.State); err != nil {
		return r, err
	}
	var commands []struct {
		Name string `json:"name"`
	}
	if err := c.Call(ctx, "query-commands", nil, &commands); err != nil {
		return r, err
	}
	for _, command := range commands {
		r.Commands[command.Name] = true
	}
	if r.Commands["query-migrate"] {
		var migration struct {
			Blockers []string `json:"blocked-reasons"`
		}
		if err := c.Call(ctx, "query-migrate", nil, &migration); err != nil {
			return r, err
		}
		r.MigrationBlockers = migration.Blockers
	}
	return r, nil
}
