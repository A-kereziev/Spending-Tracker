package main

import (
	"log"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"io/fs"
	"path/filepath"
	"strings"
	"time"

	httpz "github.com/A-kereziev/spending-tracker/http"
)

type ReceiptHandler struct {
	token     string
	ollamaSvc *OllamaService
}

func NewReceiptHandler(svc *OllamaService) *ReceiptHandler {
	token := os.Getenv("RECEIPT_API_TOKEN")

	return &ReceiptHandler{
		token:     token,
		ollamaSvc: svc,
	}
}

type Entry struct {
	Path    string          `json:"path"`
	Receipt json.RawMessage `json:"receipt"`
}

func (h *ReceiptHandler) HandleGet(w http.ResponseWriter, r *http.Request) {
	root := "/home/hydra/Spending-tracker/data/receipts"

	entries := []Entry{}
	skipped := 0

	err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
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
		rel, e := filepath.Rel(root, path)
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
}

func (h *ReceiptHandler) Handle(w http.ResponseWriter, r *http.Request) {
	shouldReturn := h.validate(w, r)
	if shouldReturn {
		return
	}

	// ============================================
	// RAW JPEG UPLOAD
	// ============================================

	defer func() {
		io.Copy(io.Discard, r.Body)
		r.Body.Close()
	}()

	// Maximum JPEG size: 2 MB
	const maxJPEGBytes int64 = 2 * 1024 * 1024

	imageData, err := io.ReadAll(
		http.MaxBytesReader(w, r.Body, maxJPEGBytes),
	)
	if err != nil {
		var maxBytesErr *http.MaxBytesError

		if errors.As(err, &maxBytesErr) {
			httpz.JSONResponse(w, http.StatusRequestEntityTooLarge, APIResponse{
				Status: "error",
				Error:  "JPEG exceeds maximum size of 2 MB",
			})
			return
		}

		httpz.JSONResponse(w, http.StatusBadRequest, APIResponse{
			Status: "error",
			Error:  "Failed to read JPEG: " + err.Error(),
		})
		return
	}

	// ============================================
	// IMAGE VALIDATION
	// ============================================

	if len(imageData) == 0 {
		httpz.JSONResponse(w, http.StatusBadRequest, APIResponse{
			Status: "error",
			Error:  "Empty JPEG data",
		})
		return
	}

	// JPEG files begin with the signature FF D8 FF
	if len(imageData) < 3 ||
		!bytes.HasPrefix(imageData, []byte{0xFF, 0xD8, 0xFF}) {

		httpz.JSONResponse(w, http.StatusBadRequest, APIResponse{
			Status: "error",
			Error:  "Invalid JPEG image signature",
		})
		return
	}

	fmt.Printf(
		"[%s] Received JPEG: %d bytes\n",
		time.Now().Format("15:04:05"),
		len(imageData),
	)

	// ============================================
	// PROCESS WITH OLLAMA
	// ============================================

	receipt, err := h.ollamaSvc.processReceipt(imageData)

	if err != nil {
		fmt.Printf(
			"[%s] Error processing: %v\n",
			time.Now().Format("15:04:05"),
			err,
		)

		httpz.JSONResponse(w, http.StatusInternalServerError, APIResponse{
			Status: "error",
			Error:  "Failed to process receipt: " + err.Error(),
		})
		return
	}

	fmt.Printf(
		"[%s] Successfully processed receipt from: %s\n",
		time.Now().Format("15:04:05"),
		receipt.Store,
	)

	// ============================================
	// SAVE RECEIPT TO DISK
	// ============================================

	receiptPath, err := h.saveReceipt(receipt)

	if err != nil {
		fmt.Printf(
			"[%s] Error saving receipt: %v\n",
			time.Now().Format("15:04:05"),
			err,
		)

		httpz.JSONResponse(w, http.StatusInternalServerError, APIResponse{
			Status:  "error",
			Error:   "Receipt processed but failed to save: " + err.Error(),
			Receipt: &receipt,
		})
		return
	}

	fmt.Printf(
		"[%s] Receipt saved to: %s\n",
		time.Now().Format("15:04:05"),
		receiptPath,
	)

	// ============================================
	// SUCCESS RESPONSE
	// ============================================

	httpz.JSONResponse(w, http.StatusOK, APIResponse{
		Status:  "success",
		Message: "Receipt processed and saved successfully",
		Receipt: &receipt,
		Bytes:   len(imageData),
		Path:    receiptPath,
	})
}

func (h *ReceiptHandler) validate(w http.ResponseWriter, r *http.Request) bool {
	if h.token == "" {
		httpz.JSONResponse(w, http.StatusInternalServerError, APIResponse{
			Status: "error",
			Error:  "Server authentication is not configured",
		})
		return true
	}

	if r.Header.Get("Authorization") != "Bearer "+h.token {
		w.Header().Set("WWW-Authenticate", "Bearer")

		httpz.JSONResponse(w, http.StatusUnauthorized, APIResponse{
			Status: "error",
			Error:  "Unauthorized",
		})
		return true
	}

	contentType := strings.TrimSpace(
		strings.Split(r.Header.Get("Content-Type"), ";")[0],
	)

	if !strings.EqualFold(contentType, "image/jpeg") {
		httpz.JSONResponse(w, http.StatusUnsupportedMediaType, APIResponse{
			Status: "error",
			Error:  "Expected Content-Type: image/jpeg",
		})

		return true
	}

	return false
}

func (*ReceiptHandler) saveReceipt(receipt Receipt) (string, error) {
	// Create directory based on receipt date
	date, err := time.Parse("2006-01-02", receipt.Date)
	if err != nil {
		return "", fmt.Errorf("invalid receipt date %q: %w", receipt.Date, err)
	}

	// Use absolute path for reliable storage
	homeDir, err := os.UserHomeDir()
	if err != nil {
		return "", fmt.Errorf("failed to get home directory: %w", err)
	}

	baseDir := filepath.Join(
		homeDir,
		"Spending-tracker",
		"data",
		"receipts",
	)

	dir := filepath.Join(
		baseDir,
		date.Format("2006"),
		date.Format("01"),
	)

	if err := os.MkdirAll(dir, 0755); err != nil {
		return "", fmt.Errorf("failed to create receipt directory: %w", err)
	}

	// Use nanosecond timestamp to avoid filename collisions
	filename := fmt.Sprintf(
		"%s_%d.json",
		receipt.Date,
		time.Now().UnixNano(),
	)

	path := filepath.Join(dir, filename)

	data, err := json.MarshalIndent(receipt, "", "  ")
	if err != nil {
		return "", fmt.Errorf("failed to encode receipt as JSON: %w", err)
	}

	if err := os.WriteFile(path, data, 0644); err != nil {
		return "", fmt.Errorf("failed to write receipt file: %w", err)
	}

	return path, nil
}
