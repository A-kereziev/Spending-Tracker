package main

import (
	"encoding/json"
	"flag"
	"io/fs"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"
)

type Entry struct {
	Path    string          `json:"path"`
	Receipt json.RawMessage `json:"receipt"`
}

func main() {
	root := flag.String("data", "data/receipts", "receipt directory")
	web := flag.String("web", "web", "frontend directory")
	addr := flag.String("addr", "127.0.0.1:8081", "listen address")
	flag.Parse()
	mux := http.NewServeMux()
	mux.HandleFunc("/api/receipts", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			w.Header().Set("Allow", "GET")
			http.Error(w, "Method not allowed", 405)
			return
		}
		entries := []Entry{}
		skipped := 0
		err := filepath.WalkDir(*root, func(path string, d fs.DirEntry, err error) error {
			if err != nil {
				return err
			}
			if d.IsDir() || !d.Type().IsRegular() || !strings.EqualFold(filepath.Ext(path), ".json") {
				return nil
			}
			b, e := os.ReadFile(path)
			if e != nil {
				return e
			}
			var obj map[string]json.RawMessage
			if json.Unmarshal(b, &obj) != nil || obj == nil || obj["store"] == nil || obj["items"] == nil {
				skipped++
				return nil
			}
			rel, e := filepath.Rel(*root, path)
			if e != nil {
				return e
			}
			entries = append(entries, Entry{filepath.ToSlash(rel), b})
			return nil
		})
		if err != nil {
			log.Print(err)
			http.Error(w, "Cannot read receipt directory", 500)
			return
		}
		w.Header().Set("Content-Type", "application/json; charset=utf-8")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(struct {
			Receipts []Entry `json:"receipts"`
			Skipped  int     `json:"skipped"`
		}{entries, skipped})
	})
	mux.Handle("/", http.FileServer(http.Dir(*web)))
	log.Printf("BillSnap: http://%s", *addr)
	log.Fatal(http.ListenAndServe(*addr, mux))
}
