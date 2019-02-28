#!/bin/sh

export GOPATH=$(pwd) # TODO get script location
go get github.com/sahilm/fuzzy
go get github.com/go-telegram-bot-api/telegram-bot-api
go get golang.org/x/net/proxy
go build

