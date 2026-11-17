//go:build windows

package main

import "os/exec"

func prepProc(c *exec.Cmd) {}

// no Windows nao ha SIGINT por processo; interromper reinicia o agente
func interrupt(c *exec.Cmd) { c.Process.Kill() }
