package search

import (
	"encoding/csv"
	"fmt"
	"io"
	"log"
	"os"
	"strconv"

	"github.com/sahilm/fuzzy"
)

type tRecord struct {
	id   uint64
	text string
}

type tRecords []tRecord

func (records tRecords) String(i int) string {
	return records[i].text
}

func (records tRecords) Len() int {
	return len(records)
}

// max_matches = 0 means unlimited
func Search(dbname string, query string, max_matches uint) []string {
	f, err := os.Open(dbname)
	defer f.Close()
	if err != nil {
		log.Panic(err)
	}
	reader := csv.NewReader(f)
	var records tRecords
	for {
		record, err := reader.Read()
		if err == io.EOF {
			break
		}
		if err != nil {
			log.Panic(err)
		}
		if len(record) < 2 {
			log.Printf("Malformed record: %v\n", record)
		}
		var id uint64
		id, err = strconv.ParseUint(record[0], 10, 64)
		if err != nil {
			log.Printf("Malformed record: %v\n", record)
		}
		records = append(records, tRecord{id, record[1]})
	}
	results := fuzzy.FindFrom(query, records)
	matches := make([]string, 0, max_matches)
	var matches_left int = int(max_matches)
	for _, r := range results {
		match := records[r.Index].id
		matches = append(matches, fmt.Sprintf("f%05d.gif", match))
		matches_left -= 1
		if max_matches > 0 && matches_left <= 0 {
			break
		}
	}
	return matches
}
