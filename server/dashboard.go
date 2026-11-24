package main

import (
	"embed"
	"encoding/json"
	"io/fs"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

//go:embed web/*
var dashboardFiles embed.FS

type dashboardEntry struct {
	Path    string          `json:"path"`
	Receipt json.RawMessage `json:"receipt"`
}

func dashboardHandler(root string) http.Handler {
	mux := http.NewServeMux()

	mux.HandleFunc("/api/receipts", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			w.Header().Set("Allow", "GET")
			http.Error(w, "Method not allowed", 405)
			return
		}

		entries := []dashboardEntry{}
		skipped := 0

		err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
			if err != nil {
				if path == root && os.IsNotExist(err) {
					return nil
				}
				return err
			}

			if d.IsDir() || !d.Type().IsRegular() ||
				!strings.EqualFold(filepath.Ext(path), ".json") {
				return nil
			}

			b, err := os.ReadFile(path)
			if err != nil {
				return err
			}

			var receipt struct {
				Store *string  `json:"store"`
				Date  *string  `json:"date"`
				Total *float64 `json:"total"`
				Items *[]struct {
					Name     *string  `json:"name"`
					Quantity *int     `json:"quantity"`
					Price    *float64 `json:"price"`
				} `json:"items"`
			}

			if json.Unmarshal(b, &receipt) != nil ||
				receipt.Store == nil ||
				receipt.Date == nil ||
				receipt.Total == nil ||
				receipt.Items == nil {

				skipped++
				return nil
			}

			if _, err := time.Parse("2006-01-02", *receipt.Date); err != nil {
				skipped++
				return nil
			}

			for _, i := range *receipt.Items {
				if i.Name == nil || i.Quantity == nil || i.Price == nil {
					skipped++
					return nil
				}
			}

			rel, err := filepath.Rel(root, path)
			if err != nil {
				return err
			}

			entries = append(entries, dashboardEntry{
				filepath.ToSlash(rel),
				b,
			})

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
			Receipts []dashboardEntry `json:"receipts"`
			Skipped  int              `json:"skipped"`
		}{entries, skipped})
	})

	assets, err := fs.Sub(dashboardFiles, "web")
	if err != nil {
		panic(err)
	}

	mux.Handle("/", http.FileServer(http.FS(assets)))

	return mux
}

func startDashboard() error {
	home, err := os.UserHomeDir()
	if err != nil {
		return err
	}

	root := filepath.Join(
		home,
		"Spending-tracker",
		"data",
		"receipts",
	)

	listener, err := net.Listen("tcp", "127.0.0.1:8081")
	if err != nil {
		return err
	}

	server := &http.Server{
		Handler:           dashboardHandler(root),
		ReadHeaderTimeout: 5 * time.Second,
		WriteTimeout:      30 * time.Second,
		IdleTimeout:       60 * time.Second,
	}

	go func() {
		log.Printf("BillSnap dashboard: http://%s", listener.Addr())

		if err := server.Serve(listener); err != nil &&
			err != http.ErrServerClosed {
			log.Print(err)
		}
	}()

	return nil
}