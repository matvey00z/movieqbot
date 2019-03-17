package search

import (
	"database/sql"
	"log"

	_ "github.com/mattn/go-sqlite3"
	"github.com/sahilm/fuzzy"
)

type Record struct {
	Id       uint64
	Name     string
	Text     string
	TgFileId *string
}

type tRecords []Record

func (records tRecords) String(i int) string {
	return records[i].Text
}

func (records tRecords) Len() int {
	return len(records)
}

// max_matches = 0 means unlimited
func Search(dbname string, query string, max_matches uint) []string {
	extended := SearchEx(dbname, query, max_matches)
	ret := make([]string, len(extended))
	for i, ex := range extended {
		ret[i] = ex.Name
	}
	return ret
}

func SearchEx(dbname string, query string, max_matches uint) []Record {
	db, err := sql.Open("sqlite3", dbname)
	if err != nil {
		log.Panic(err)
	}
	defer db.Close()
	var records tRecords
	rows, err := db.Query(`
		SELECT id, name, text, tg_file_id
		FROM gifs`)
	if err != nil {
		log.Panic(err)
	}
	defer rows.Close()
	for rows.Next() {
		var record Record
		err := rows.Scan(&record.Id, &record.Name, &record.Text,
			&record.TgFileId)
		if err != nil {
			log.Panic(err)
		}
		records = append(records, record)
	}
	results := fuzzy.FindFrom(query, records)
	matches := make([]Record, 0, max_matches)
	var matches_left int = int(max_matches)
	for _, r := range results {
		match := records[r.Index]
		matches = append(matches, match)
		matches_left -= 1
		if max_matches > 0 && matches_left <= 0 {
			break
		}
	}
	return matches
}
