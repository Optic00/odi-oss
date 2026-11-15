// SPDX-FileCopyrightText: 2026 The odi-oss contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// omci-libcheck exports the managed entity definitions of omci-lib-go as
// JSON, so check.py can hold them against src/omci/generated/omci_mib.json.
// It is a development tool only; nothing here is built into the image.
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"sort"

	me "github.com/opencord/omci-lib-go/v2/generated"
)

type attr struct {
	Index      uint   `json:"index"`
	Name       string `json:"name"`
	Size       int    `json:"size"`
	Access     string `json:"access"`
	Table      bool   `json:"table"`
	Optional   bool   `json:"optional"`
	Deprecated bool   `json:"deprecated"`
	Avc        bool   `json:"avc"`
}

type entity struct {
	ClassID uint16 `json:"classId"`
	Name    string `json:"name"`
	Attrs   []attr `json:"attrs"`
}

func access(a me.AttributeDefinition) string {
	s := ""
	for _, f := range []struct {
		acc me.AttributeAccess
		c   string
	}{{me.Read, "R"}, {me.Write, "W"}, {me.SetByCreate, "C"}} {
		if a.GetAccess().Contains(f.acc) {
			s += f.c
		}
	}
	return s
}

func main() {
	ids := me.GetSupportedClassIDs()
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	var out []entity
	for _, id := range ids {
		m, err := me.LoadManagedEntityDefinition(id)
		if err.StatusCode() != me.Success {
			fmt.Fprintf(os.Stderr, "class %d: %v\n", id, err)
			os.Exit(1)
		}
		e := entity{ClassID: uint16(id), Name: m.GetName()}
		defs := m.GetAttributeDefinitions()
		for _, k := range me.GetAttributeDefinitionMapKeys(defs) {
			a := defs[k]
			e.Attrs = append(e.Attrs, attr{
				Index: a.GetIndex(), Name: a.GetName(), Size: a.GetSize(),
				Access: access(a), Table: a.IsTableAttribute(),
				Optional: a.Optional, Deprecated: a.Deprecated, Avc: a.Avc,
			})
		}
		out = append(out, e)
	}
	enc := json.NewEncoder(os.Stdout)
	enc.SetIndent("", " ")
	if err := enc.Encode(out); err != nil {
		os.Exit(1)
	}
}
