#!/bin/sh

export GOPATH=$(pwd)
go get github.com/sahilm/fuzzy
go get github.com/mattn/go-sqlite3

pkg_path="$(dirname "$0"})"
# go build requires the path to be relative
rel_path="$(realpath --relative-to="$(pwd)" "$pkg_path")"
go build -i "$rel_path"
