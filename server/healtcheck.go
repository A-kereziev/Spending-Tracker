package main

import (
	"net/http"

	httpz "github.com/A-kereziev/spending-tracker/http"
)

type HealthCheck struct{}

func NewHealthCheck() *HealthCheck {
	return &HealthCheck{}
}

func (*HealthCheck) Handle(w http.ResponseWriter, r *http.Request) {
	httpz.JSONResponse(w, http.StatusOK, APIResponse{
		Status:  "ok",
		Message: "Receipt scanner API is running",
	})
}
