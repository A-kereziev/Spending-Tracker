package main

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"

	httpz "github.com/A-kereziev/spending-tracker/http"
)

type OllamaService struct{}

func NewOllamaService() *OllamaService {
	return &OllamaService{}
}

func (o *OllamaService) processReceipt(imageData []byte) (Receipt, error) {
	imageBase64 := base64.StdEncoding.EncodeToString(imageData)

	// ============================================================
	// PASS 1: LITERAL OCR
	// ============================================================

	ocrPrompt := `
Perform literal OCR on this Bulgarian receipt.

Transcribe ONLY text that is visibly present.

Rules:
- Copy Bulgarian Cyrillic exactly.
- Do not translate.
- Do not correct spelling.
- Do not guess missing characters.
- Do not invent text.
- Preserve numbers and decimal values exactly.
- Preserve the order and line structure.
- Copy product names exactly as printed.
- Copy quantity/unit-price lines exactly.
- Do not interpret or calculate anything.
- If a character cannot be read, use ?.

Return ONLY the transcribed receipt text.
No JSON.
No Markdown.
No explanation.
`

	ocrText, err := o.callOllamaVision(
		"qwen2.5vl-receipt",
		ocrPrompt,
		imageBase64,
	)
	if err != nil {
		return Receipt{}, fmt.Errorf("OCR pass failed: %w", err)
	}

	fmt.Println()
	fmt.Println("===== RAW OCR =====")
	fmt.Println(ocrText)
	fmt.Println("===================")

	// ============================================================
	// PASS 2: STRUCTURE THE OCR
	// ============================================================

	structurePrompt := fmt.Sprintf(`
You are a receipt data extraction engine.

You are given OCR text from a Bulgarian Lidl receipt.

Your job is to convert ONLY the information present in that OCR text
into structured JSON.

IMPORTANT:

DO NOT perform OCR yourself.

DO NOT invent missing text.

DO NOT correct spelling.

DO NOT "fix" product names.

DO NOT replace strange-looking Bulgarian words with words that seem
more likely.

The OCR text is authoritative.

If the OCR says a product name is:

Мяка питка

then the product name MUST remain:

Мяка питка

even if you think this is unusual.

Do NOT change it to something you think is more reasonable.

============================================================
RECEIPT RULES
============================================================

STORE:

Extract the store name from the OCR text.

DATE:

Find the printed receipt date.

The receipt normally contains a date in:

DD.MM.YYYY HH:MM:SS

Convert ONLY that date to:

YYYY-MM-DD

Do not invent a date.

ITEMS:

Extract each actual purchased product.

A product-name line represents one purchased item.

Lines such as:

12.000 x 0.59
2.000 x 1.89
2.000 x 1.49

are NOT separate products.

They are supporting quantity/unit-price information belonging to
the product above them.

PRICE:

Use the FINAL PRICE of the product.

Do not use the unit price.

Do not use VAT.

Do not use discounts unless the receipt clearly shows that the
discounted value is the final item price.

QUANTITY:

Use the quantity when explicitly shown.

If there is no reliable quantity information for an item,
use 1.

TOTAL:

Extract the value associated with:

ОБЩА СУМА ЛВ

Do not calculate the total yourself.

============================================================
OCR TEXT
============================================================

%s

============================================================

Return ONLY valid JSON.

Use exactly:

{
  "store": "string",
  "date": "YYYY-MM-DD",
  "items": [
    {
      "name": "string",
      "quantity": 1,
      "price": 0.00
    }
  ],
  "total": 0.00
}

Do not use Markdown.
Do not use triple backticks.
Do not include explanations.
`, ocrText)

	jsonFormat := json.RawMessage(`{
                "type": "object",
                "properties": {
                        "store": {
                                "type": "string"
                        },
                        "date": {
                                "type": "string"
                        },
                        "items": {
                                "type": "array",
                                "items": {
                                        "type": "object",
                                        "properties": {
                                                "name": {
                                                        "type": "string"
                                                },
                                                "quantity": {
                                                        "type": "integer"
                                                },
                                                "price": {
                                                        "type": "number"
                                                }
                                        },
                                        "required": ["name", "quantity", "price"]
                                }
                        },
                        "total": {
                                "type": "number"
                        }
                },
                "required": ["store", "date", "items", "total"]
        }`)

	jsonResponse, err := o.callOllamaText(
		"qwen2.5vl-receipt",
		structurePrompt,
		jsonFormat,
	)

	if err != nil {
		return Receipt{}, fmt.Errorf("structure pass failed: %w", err)
	}

	fmt.Println()
	fmt.Println("===== STRUCTURED RESPONSE =====")
	fmt.Println(jsonResponse)
	fmt.Println("================================")

	cleanedJSON, err := httpz.ExtractJSON(jsonResponse)
	if err != nil {
		return Receipt{}, fmt.Errorf(
			"could not extract JSON from model response: %w",
			err,
		)
	}

	var receipt Receipt

	err = json.Unmarshal([]byte(cleanedJSON), &receipt)
	if err != nil {
		return Receipt{}, fmt.Errorf(
			"model returned invalid JSON: %w\nJSON: %s",
			err,
			cleanedJSON,
		)
	}

	return receipt, nil
}

func (*OllamaService) callOllamaVision(
	model string,
	prompt string,
	imageBase64 string,
) (string, error) {

	requestBody := OllamaRequest{
		Model:  model,
		Stream: false,
		Messages: []Message{
			{
				Role:    "user",
				Content: prompt,
				Images:  []string{imageBase64},
			},
		},
	}

	jsonData, err := json.Marshal(requestBody)
	if err != nil {
		return "", fmt.Errorf("failed to create vision request: %w", err)
	}

	fmt.Println("Sending image to Ollama for OCR...")

	resp, err := http.Post(
		"http://localhost:11434/api/chat",
		"application/json",
		bytes.NewBuffer(jsonData),
	)

	if err != nil {
		return "", fmt.Errorf("failed to connect to Ollama: %w", err)
	}

	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)

		return "", fmt.Errorf(
			"Ollama returned HTTP %d: %s",
			resp.StatusCode,
			string(body),
		)
	}

	responseData, err := io.ReadAll(resp.Body)
	if err != nil {
		return "", fmt.Errorf("failed to read Ollama response: %w", err)
	}

	var ollamaResponse OllamaResponse

	err = json.Unmarshal(responseData, &ollamaResponse)
	if err != nil {
		return "", fmt.Errorf(
			"failed to parse Ollama response: %w\nResponse: %s",
			err,
			string(responseData),
		)
	}

	return ollamaResponse.Message.Content, nil
}

func (*OllamaService) callOllamaText(
	model string,
	prompt string,
	format json.RawMessage,
) (string, error) {
	requestBody := OllamaRequest{
		Model:  model,
		Stream: false,
		Format: format,
		Messages: []Message{
			{
				Role:    "user",
				Content: prompt,
			},
		},
	}

	jsonData, err := json.Marshal(requestBody)
	if err != nil {
		return "", fmt.Errorf("failed to create text request: %w", err)
	}

	fmt.Println("Sending OCR text to Ollama for structuring...")

	resp, err := http.Post(
		"http://localhost:11434/api/chat",
		"application/json",
		bytes.NewBuffer(jsonData),
	)

	if err != nil {
		return "", fmt.Errorf("failed to connect to Ollama: %w", err)
	}

	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)

		return "", fmt.Errorf(
			"ollama returned HTTP %d: %s",
			resp.StatusCode,
			string(body),
		)
	}

	responseData, err := io.ReadAll(resp.Body)
	if err != nil {
		return "", fmt.Errorf("failed to read Ollama response: %w", err)
	}

	var ollamaResponse OllamaResponse

	err = json.Unmarshal(responseData, &ollamaResponse)
	if err != nil {
		return "", fmt.Errorf(
			"failed to parse Ollama response: %w\nResponse: %s",
			err,
			string(responseData),
		)
	}

	return ollamaResponse.Message.Content, nil
}
