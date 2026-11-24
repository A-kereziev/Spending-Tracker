package main

import (
	"crypto/tls"
	"fmt"
	"log"
	"net/http"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
)

func main() {
	mux := chi.NewRouter()
	mux.Use(middleware.Logger)

	ollamaSvc := NewOllamaService()
	receiptHandler := NewReceiptHandler(ollamaSvc)
	healtCheck := NewHealthCheck()

	mux.Get("/health", healtCheck.Handle)
	mux.Post("/api/receipt", receiptHandler.Handle)

	svr := http.Server{
		Addr:              "0.0.0.0:8080",
		Handler:           mux,
		ReadTimeout:       time.Second * 2,
		ReadHeaderTimeout: time.Second * 2,
		WriteTimeout:      time.Second * 2,
		IdleTimeout:       0,
		MaxHeaderBytes:    0,
		TLSNextProto:      map[string]func(*http.Server, *tls.Conn, http.Handler){},
	}

	fmt.Printf("=== Receipt Scanner API Server ===\n")
	fmt.Printf("Server starting on %s\n\n", svr.Addr)

	fmt.Printf("Endpoints:\n")
	fmt.Printf("  POST /api/receipt  - Send raw JPEG binary for processing\n")
	fmt.Printf("  GET  /health       - Health check\n")

	fmt.Printf("================================\n")

	err := svr.ListenAndServe()
	if err != nil {
		log.Fatal(err)
	}
}
