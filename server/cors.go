package main

import (
	"net/http"
	"strings"
)

type CORSConfig struct {
	AllowedOrigins   []string
	AllowedMethods   []string
	AllowedHeaders   []string
	ExposedHeaders   []string
	AllowCredentials bool
	MaxAge           int
}

func CORS(cfg CORSConfig) func(http.Handler) http.Handler {
	origins := make(map[string]struct{}, len(cfg.AllowedOrigins))
	for _, origin := range cfg.AllowedOrigins {
		origins[origin] = struct{}{}
	}

	methods := strings.Join(cfg.AllowedMethods, ", ")
	headers := strings.Join(cfg.AllowedHeaders, ", ")
	exposed := strings.Join(cfg.ExposedHeaders, ", ")

	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			origin := r.Header.Get("Origin")

			_, allowed := origins[origin]

			if _, wildcard := origins["*"]; wildcard {
				allowed = true
			}

			if !allowed {
				next.ServeHTTP(w, r)
				return
			}

			if _, wildcard := origins["*"]; wildcard && !cfg.AllowCredentials {
				w.Header().Set("Access-Control-Allow-Origin", "*")
			} else {
				w.Header().Set("Access-Control-Allow-Origin", origin)
				w.Header().Add("Vary", "Origin")
			}

			if cfg.AllowCredentials {
				w.Header().Set("Access-Control-Allow-Credentials", "true")
			}

			if exposed != "" {
				w.Header().Set("Access-Control-Expose-Headers", exposed)
			}

			if r.Method == http.MethodOptions {
				if methods != "" {
					w.Header().Set("Access-Control-Allow-Methods", methods)
				}

				if headers != "" {
					w.Header().Set("Access-Control-Allow-Headers", headers)
				}

				if cfg.MaxAge > 0 {
					w.Header().Set(
						"Access-Control-Max-Age",
						strings.TrimSpace(strings.Repeat("", 0))+http.StatusText(0),
					)
				}

				w.WriteHeader(http.StatusNoContent)
				return
			}

			next.ServeHTTP(w, r)
		})
	}
}

