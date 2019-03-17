package main

import (
	"../search"
	"bufio"
	"database/sql"
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

const placeholder = ":("

type tBot struct {
	api           *tgbotapi.BotAPI
	dbname        string
	authList      map[int64]struct{}
	db            *sql.DB
	serviceChatId int64
}

func (bot *tBot) openDB() {
	var err error
	bot.db, err = sql.Open("sqlite3", bot.dbname)
	if err != nil {
		log.Panic(err)
	}
}

func (bot *tBot) closeDB() {
	bot.db.Close()
}

func (bot *tBot) updateFileId(id uint64, fileId string) {
	_, err := bot.db.Exec(`
		UPDATE gifs
		SET tg_file_id = ?
		WHERE id = ?`, fileId, id)
	if err != nil {
		log.Panic(err)
	}
}

func (bot *tBot) handleCommand(update *tgbotapi.Update) bool {
	command := update.Message.Command()
	if command == "" {
		return false
	}
	if command == "ping" {
		msg := tgbotapi.NewMessage(update.Message.Chat.ID,
			"pong")
		bot.api.Send(msg)
	} else {
		msg := tgbotapi.NewMessage(update.Message.Chat.ID,
			placeholder)
		msg.ParseMode = "Markdown"
		bot.api.Send(msg)
	}
	return true
}

func (bot *tBot) handleQuery(update *tgbotapi.Update) {
	results := search.SearchEx(bot.dbname, update.Message.Text, max_matches)
	msg := tgbotapi.NewMessage(update.Message.Chat.ID,
		fmt.Sprintf("Нашёл %v, ща загружу, абажди", len(results)))
	bot.api.Send(msg)
	for _, result := range results {
		var animation tgbotapi.AnimationConfig
		shared := false
		if result.TgFileId != nil && *result.TgFileId != "" {
			animation = tgbotapi.NewAnimationShare(update.Message.Chat.ID,
				*result.TgFileId)
			shared = true
		} else {
			animation = tgbotapi.NewAnimationUpload(update.Message.Chat.ID,
				"gifs/"+result.Name)
		}
		resp, err := bot.api.Send(animation)
		if err != nil {
			log.Panic(err)
		}
		if !shared {
			fileId := resp.Animation.FileID
			bot.updateFileId(result.Id, fileId)
		}
	}
}

func (bot *tBot) handleInlineQuery(update *tgbotapi.Update) {
	if !bot.auth(int64(update.InlineQuery.From.ID)) {
		resp := tgbotapi.NewInlineQueryResultArticle(update.InlineQuery.ID,
			"You're not authenticated", update.InlineQuery.Query)
		resp.Description = "You're not authenticated"
		respConf := tgbotapi.InlineConfig{
			InlineQueryID: update.InlineQuery.ID,
			Results:       []interface{}{resp},
			IsPersonal:    false,
		}
		_, err := bot.api.AnswerInlineQuery(respConf)
		if err != nil {
			log.Panic(err)
		}
		return
	}
	results := search.SearchEx(bot.dbname, update.InlineQuery.Query, max_matches)
	respConf := tgbotapi.InlineConfig{
		InlineQueryID: update.InlineQuery.ID,
		IsPersonal:    false,
	}
	for id, result := range results {
		if result.TgFileId == nil || *result.TgFileId == "" {
			animation := tgbotapi.NewAnimationUpload(bot.serviceChatId,
				"gifs/"+result.Name)
			resp, err := bot.api.Send(animation)
			if err != nil {
				log.Panic(err)
			}
			fileId := resp.Animation.FileID
			bot.updateFileId(result.Id, fileId)
			result.TgFileId = &fileId
		}
		gif := tgbotapi.NewInlineQueryResultCachedMPEG4GIF(strconv.Itoa(id),
			*result.TgFileId)
		respConf.Results = append(respConf.Results, gif)
	}
	_, err := bot.api.AnswerInlineQuery(respConf)
	if err != nil {
		log.Panic(err)
	}
}

func (bot *tBot) auth(id int64) bool {
	_, ok := bot.authList[id]
	return ok
}

func (bot *tBot) serve(token string, proxyAddr string) {
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
	var err error
	bot.api, err = tgbotapi.NewBotAPIWithClient(token, client)
	if err != nil {
		log.Panic(err)
	}
	update_config := tgbotapi.NewUpdate(0)
	update_config.Timeout = 60
	updates, err := bot.api.GetUpdatesChan(update_config)
	for update := range updates {
		if update.InlineQuery != nil {
			bot.handleInlineQuery(&update)
			continue
		}
		if update.Message == nil { // ignore any non-Message Updates
			continue
		}
		if !bot.auth(int64(update.Message.From.ID)) {
			msg := tgbotapi.NewMessage(update.Message.Chat.ID,
				"You're not authenticated")
			bot.api.Send(msg)
			continue
		}
		if bot.handleCommand(&update) {
			continue
		}
		if update.Message.Text == "" { // ignore non-text messages
			continue
		}
		bot.handleQuery(&update)
	}
}

func (bot *tBot) getAuthList(authListFileName string) {
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
		bot.authList[id] = struct{}{}
	}
	if err := scanner.Err(); err != nil {
		log.Panic(err)
	}
}

func main() {
	var token string
	var proxyAddr string
	var authListFileName string
	bot := tBot{
		authList: make(map[int64]struct{}),
	}
	flag.StringVar(&bot.dbname, "dbname", "", "Database filename")
	flag.StringVar(&token, "token", "", "Bot token")
	flag.StringVar(&proxyAddr, "proxy", "", "SOCKS5 proxy address")
	flag.StringVar(&authListFileName, "auth", "", "Authentication file")
	flag.Int64Var(&bot.serviceChatId, "service_chat", -1, "Service Chat ID")
	flag.Parse()
	if bot.dbname == "" || token == "" || authListFileName == "" ||
		bot.serviceChatId <= 0 {
		flag.Usage()
		return
	}
	bot.openDB()
	defer bot.closeDB()
	bot.getAuthList(authListFileName)
	bot.serve(token, proxyAddr)
}
