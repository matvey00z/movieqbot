package search

import (
	"database/sql"
	"log"

	_ "github.com/mattn/go-sqlite3"
	"github.com/sahilm/fuzzy"
)

type tRecord struct {
	id   uint64
	name string
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
	db, err := sql.Open("sqlite3", dbname)
	if err != nil {
		log.Panic(err)
	}
	defer db.Close()
	var records tRecords
	rows, err := db.Query(`
		SELECT id, name, text
		FROM gifs`)
	if err != nil {
		log.Panic(err)
	}
	defer rows.Close()
	for rows.Next() {
		var record tRecord
		err := rows.Scan(&record.id, &record.name, &record.text)
		if err != nil {
			log.Panic(err)
		}
		records = append(records, record)
	}
	results := fuzzy.FindFrom(query, records)
	matches := make([]string, 0, max_matches)
	var matches_left int = int(max_matches)
	for _, r := range results {
		match := records[r.Index].name
		matches = append(matches, match)
		matches_left -= 1
		if max_matches > 0 && matches_left <= 0 {
			break
		}
	}
	return matches
}
