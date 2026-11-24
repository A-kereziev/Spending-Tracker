package main

import "encoding/json"

type OllamaRequest struct {
	Model    string          `json:"model"`
	Messages []Message       `json:"messages"`
	Stream   bool            `json:"stream"`
	Format   json.RawMessage `json:"format,omitempty"`
}

type Message struct {
	Role    string   `json:"role"`
	Content string   `json:"content"`
	Images  []string `json:"images,omitempty"`
}

type OllamaResponse struct {
	Message Message `json:"message"`
}

type Receipt struct {
	Store string  `json:"store"`
	Date  string  `json:"date"`
	Items []Item  `json:"items"`
	Total float64 `json:"total"`
}

type Item struct {
	Name     string  `json:"name"`
	Quantity int     `json:"quantity"`
	Price    float64 `json:"price"`
}

type APIResponse struct {
	Status  string   `json:"status"`
	Message string   `json:"message,omitempty"`
	Receipt *Receipt `json:"receipt,omitempty"`
	Bytes   int      `json:"bytes,omitempty"`
	Error   string   `json:"error,omitempty"`
	Path    string   `json:"path,omitempty"`
}
