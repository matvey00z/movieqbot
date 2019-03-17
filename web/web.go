package main

import (
	"../search"
	"flag"
	"html/template"
	"io/ioutil"
	"log"
	"net/http"
	"os"
)

var placeholder []byte = []byte("Unknown server error")

func indexHandler(w http.ResponseWriter, r *http.Request) {
	page, err := ioutil.ReadFile("index.html")
	if err != nil {
		page = placeholder
	}
	w.Write(page)
}

type tSearchParams struct {
	Query   string
	Results []string
}

func searchHandler(w http.ResponseWriter, r *http.Request) {
	err := r.ParseForm()
	if err != nil {
		w.Write(placeholder)
		return
	}
	query := r.FormValue("query")
	if query == "" {
		w.Write(placeholder)
		return
	}
	params := tSearchParams{
		Query: query,
	}
	// TODO get directory path from cmd
	results := search.Search("gifs/db.csv", query, 0)
	for _, fname := range results {
		params.Results = append(params.Results, "gifs/"+fname)
	}
	page, err := template.ParseFiles("search.html")
	if err != nil {
		w.Write(placeholder)
		return
	}
	if err = page.Execute(w, params); err != nil {
		w.Write(placeholder)
		return
	}
}

func main() {
	var workDir string
	flag.StringVar(&workDir, "dir", "", "Working directory")
	flag.Parse()
	if workDir != "" {
		if err := os.Chdir(workDir); err != nil {
			log.Panic(err)
		}
	}
	servemux := http.NewServeMux()
	servemux.HandleFunc("/", indexHandler)
	servemux.HandleFunc("/search", searchHandler)
	servemux.Handle("/gifs/", http.FileServer(http.Dir(".")))
	server := &http.Server{
		Addr:    ":8080",
		Handler: servemux,
	}
	log.Println("Start serving")
	log.Fatal(server.ListenAndServe())
}
