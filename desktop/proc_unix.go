//go:build !windows

package main

import (
	"os/exec"
	"syscall"
)

func prepProc(c *exec.Cmd) { c.SysProcAttr = &syscall.SysProcAttr{Setpgid: true} }

// Ctrl+C no agente: interrompe a resposta em andamento
func interrupt(c *exec.Cmd) { syscall.Kill(-c.Process.Pid, syscall.SIGINT) }
