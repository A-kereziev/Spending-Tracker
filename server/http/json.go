package http

import (
	"encoding/json"
	"fmt"
	"strings"
)

func ExtractJSON(text string) (string, error) {
	text = strings.TrimSpace(text)

	if json.Valid([]byte(text)) {
		return text, nil
	}

	if strings.Contains(text, "```") {
		parts := strings.Split(text, "```")

		for _, part := range parts {
			part = strings.TrimSpace(part)

			if strings.HasPrefix(strings.ToLower(part), "json") {
				part = strings.TrimSpace(part[4:])
			}

			if json.Valid([]byte(part)) {
				return part, nil
			}
		}
	}

	start := strings.Index(text, "{")
	end := strings.LastIndex(text, "}")

	if start >= 0 && end > start {
		candidate := strings.TrimSpace(text[start : end+1])

		if json.Valid([]byte(candidate)) {
			return candidate, nil
		}
	}

	return "", fmt.Errorf("no valid JSON object found in model response")
}
