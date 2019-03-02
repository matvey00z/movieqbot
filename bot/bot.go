package main

import (
	"../search"
	"bufio"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"strconv"

	"github.com/go-telegram-bot-api/telegram-bot-api"
	"golang.org/x/net/proxy"
)

const (
	max_matches = 5
)

var authList = make(map[int64]struct{})

const placeholder = "```" + `
....................................хуй.......
хуй........хуй..хуй......хуй..хуй.......хуйхуй
.хуй....хуй......хуй....хуй...хуй.....хуй..хуй
..хуй..хуй........хуй..хуй....хуй....хуй...хуй
...хуйхуй..........хуйхуй.....хуй...хуй....хуй
..хуй..хуй............хуй.....хуй..хуй.....хуй
.хуй....хуй..........хуй......хуй.хуй......хуй
хуй......хуй........хуй.......хуйхуй.......хуй
` + "```"

func serve(dbname string, token string, proxyAddr string) {
	var client *http.Client = nil
	if proxyAddr != "" {
		dialer, err := proxy.SOCKS5("tcp", proxyAddr, nil, proxy.Direct)
		if err != nil {
			log.Panic(err)
		}
		transport := &http.Transport{
			Dial: dialer.Dial,
		}
		client = &http.Client{
			Transport: transport,
		}
	} else {
		client = &http.Client{}
	}
	bot, err := tgbotapi.NewBotAPIWithClient(token, client)
	if err != nil {
		log.Panic(err)
	}
	update_config := tgbotapi.NewUpdate(0)
	update_config.Timeout = 60
	updates, err := bot.GetUpdatesChan(update_config)
	for update := range updates {
		if _, ok := authList[int64(update.Message.From.ID)]; !ok {
			msg := tgbotapi.NewMessage(update.Message.Chat.ID,
				"You're not authenticated")
			bot.Send(msg)
			continue
		}
		if update.Message == nil { // ignore any non-Message Updates
			continue
		}
		command := update.Message.Command()
		if command != "" {
			if command == "ping" {
				msg := tgbotapi.NewMessage(update.Message.Chat.ID,
					"pong")
				bot.Send(msg)
			} else {
				msg := tgbotapi.NewMessage(update.Message.Chat.ID,
					placeholder)
				msg.ParseMode = "Markdown"
				bot.Send(msg)
			}
			continue
		}
		if update.Message.Text == "" { // ignore non-text messages
			continue
		}
		results := search.Search(dbname, update.Message.Text, max_matches)
		msg := tgbotapi.NewMessage(update.Message.Chat.ID,
			fmt.Sprintf("Нашёл %v, ща загружу, абажди", len(results)))
		bot.Send(msg)
		for _, fname := range results {
			animation := tgbotapi.NewAnimationUpload(update.Message.Chat.ID,
				"gifs/"+fname)
			bot.Send(animation)
		}
	}
}

func getAuthList(authListFileName string) {
	file, err := os.Open(authListFileName)
	if err != nil {
		log.Panic(err)
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		line := scanner.Text()
		id, err := strconv.ParseInt(line, 10, 64)
		if err != nil {
			log.Panic(err)
		}
		authList[id] = struct{}{}
	}
	if err := scanner.Err(); err != nil {
		log.Panic(err)
	}
}

func main() {
	var dbname string
	var token string
	var proxyAddr string
	var authListFileName string
	flag.StringVar(&dbname, "dbname", "", "Database filename")
	flag.StringVar(&token, "token", "", "Bot token")
	flag.StringVar(&proxyAddr, "proxy", "", "SOCKS5 proxy address")
	flag.StringVar(&authListFileName, "auth", "", "Authentication file")
	flag.Parse()
	if dbname == "" || token == "" || authListFileName == "" {
		flag.Usage()
		return
	}
	getAuthList(authListFileName)
	serve(dbname, token, proxyAddr)
}
