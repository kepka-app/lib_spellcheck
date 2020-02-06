// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//

#include "spellcheck/spellcheck_hunspell.h"

#include "hunspell/hunspell.hxx"
#include "spellcheck/spellcheck_value.h"

#include <QFileInfo>
#include <QTextCodec>

namespace Platform::Spellchecker {
namespace {

using WordsMap = std::map<QChar::Script, std::vector<QString>>;

// Maximum number of words in the custom spellcheck dictionary.
constexpr auto kMaxSyncableDictionaryWords = 1300;

#ifdef Q_OS_WIN
const auto kLineBreak = QByteArrayLiteral("\r\n");
#else // Q_OS_WIN
const auto kLineBreak = QByteArrayLiteral("\n");
#endif // Q_OS_WIN

auto LocaleNameFromLangId(int langId) {
	return ::Spellchecker::LocaleFromLangId(langId).name();
}

QString CustomDictionaryPath() {
	return QStringLiteral("%1/%2")
		.arg(::Spellchecker::WorkingDirPath())
		.arg("custom");
}

class HunspellEngine {
public:
	HunspellEngine(const QString &lang);
	~HunspellEngine() = default;

	bool isValid() const;

	bool spell(const QString &word) const;

	void suggest(
		const QString &wrongWord,
		std::vector<QString> *optionalSuggestions);

	QString lang();
	QChar::Script script();

	HunspellEngine(const HunspellEngine &) = delete;
	HunspellEngine &operator=(const HunspellEngine &) = delete;

private:
	QString _lang;
	QChar::Script _script;
	std::unique_ptr<Hunspell> _hunspell;
	QTextCodec *_codec;

};


class HunspellService {
public:
	HunspellService();
	~HunspellService() = default;

	void updateLanguages(std::vector<QString> langs);
	std::vector<QString> activeLanguages();
	[[nodiscard]] bool checkSpelling(const QString &wordToCheck);

	void fillSuggestionList(
		const QString &wrongWord,
		std::vector<QString> *optionalSuggestions);

	void addWord(const QString &word);
	void removeWord(const QString &word);
	void ignoreWord(const QString &word);
	bool isWordInDictionary(const QString &word);

private:
	void writeToFile();
	void readFile();

	std::vector<QString> &addedWords(const QString &word);

	std::vector<std::unique_ptr<HunspellEngine>> _engines;
	WordsMap _ignoredWords;
	WordsMap _addedWords;

};

HunspellEngine::HunspellEngine(const QString &lang)
: _lang(lang)
, _script(::Spellchecker::LocaleToScriptCode(lang))
, _hunspell(nullptr)
, _codec(nullptr) {
	const auto workingDir = ::Spellchecker::WorkingDirPath();
	if (workingDir.isEmpty()) {
		return;
	}
	const auto dictPath = QString("%1/%2/%2")
		.arg(workingDir)
		.arg(lang)
		.toUtf8();

	const auto affPath = dictPath + ".aff";
	const auto dicPath = dictPath + ".dic";

	if (!QFileInfo(affPath).isFile() || !QFileInfo(dicPath).isFile()) {
		return;
	}

	_hunspell = std::make_unique<Hunspell>(affPath, dicPath);
	_codec = QTextCodec::codecForName(_hunspell->get_dic_encoding());
	if (!_codec) {
		_hunspell.reset();
	}
}

bool HunspellEngine::isValid() const {
	return _hunspell != nullptr;
}

bool HunspellEngine::spell(const QString &word) const {
	return _hunspell->spell(_codec->fromUnicode(word).toStdString());
}

void HunspellEngine::suggest(
	const QString &wrongWord,
	std::vector<QString> *optionalSuggestions) {
	const auto stdWord = _codec->fromUnicode(wrongWord).toStdString();

	for (const auto &guess : _hunspell->suggest(stdWord)) {
		if (optionalSuggestions->size()	== kMaxSuggestions) {
			return;
		}
		optionalSuggestions->push_back(_codec->toUnicode(
			guess.data(),
			guess.length()));
	}
}

QString HunspellEngine::lang() {
	return _lang;
}

QChar::Script HunspellEngine::script() {
	return _script;
}

std::vector<QString> HunspellService::activeLanguages() {
	return ranges::view::all(
		_engines
	) | ranges::views::filter([](auto &engine) {
		// Sometimes dictionaries may haven't enough time
		// to unload from memory.
		return engine != nullptr;
	}) | ranges::views::transform(&HunspellEngine::lang)
	| ranges::to_vector;
}

HunspellService::HunspellService() {
	readFile();
}

std::vector<QString> &HunspellService::addedWords(const QString &word) {
	return _addedWords[::Spellchecker::WordScript(&word)];
}

void HunspellService::updateLanguages(std::vector<QString> langs) {
	// Removed disabled engines.
	_engines = ranges::view::all(
		_engines
	) | ranges::views::filter([&](auto &engine) {
		// All filtered objects will be automatically deleted.
		return ranges::contains(langs, engine->lang());
	}) | ranges::views::transform([](auto &engine) {
		return std::move(engine);
	}) | ranges::to_vector;

	// Added new enabled engines.
	const auto &&stringLanguages = ranges::view::all(
		_engines
	) | ranges::views::transform(&HunspellEngine::lang);

	const auto missingLanguages = ranges::view::all(
		langs
	) | ranges::views::filter([&](auto &lang) {
		return !ranges::contains(stringLanguages, lang);
	}) | ranges::to_vector;

	_engines.reserve(_engines.size() + missingLanguages.size());

	ranges::for_each(missingLanguages, [&](auto &lang) {
		auto engine = std::make_unique<HunspellEngine>(lang);
		if (engine->isValid()) {
			_engines.push_back(std::move(engine));
		}
	});
}

bool HunspellService::checkSpelling(const QString &wordToCheck) {
	const auto wordScript = ::Spellchecker::WordScript(&wordToCheck);
	if (ranges::contains(_ignoredWords[wordScript], wordToCheck)) {
		return true;
	}
	if (ranges::contains(_addedWords[wordScript], wordToCheck)) {
		return true;
	}
	for (const auto &engine : _engines) {
		if (wordScript != engine->script()) {
			continue;
		}
		if (engine->spell(wordToCheck)) {
			return true;
		}
	}

	return false;
}

void HunspellService::fillSuggestionList(
	const QString &wrongWord,
	std::vector<QString> *optionalSuggestions) {
	const auto wordScript = ::Spellchecker::WordScript(&wrongWord);
	for (const auto &engine : _engines) {
		if (wordScript != engine->script()) {
			continue;
		}
		if (optionalSuggestions->size()	== kMaxSuggestions) {
			return;
		}
		engine->suggest(wrongWord, optionalSuggestions);
	}
}

void HunspellService::ignoreWord(const QString &word) {
	const auto wordScript = ::Spellchecker::WordScript(&word);
	_ignoredWords[wordScript].push_back(word);
}

bool HunspellService::isWordInDictionary(const QString &word) {
	return ranges::contains(addedWords(word), word);
}

void HunspellService::addWord(const QString &word) {
	const auto count = ranges::accumulate(
		ranges::view::values(_addedWords),
		0,
		ranges::plus(),
		&std::vector<QString>::size);
	if (count > kMaxSyncableDictionaryWords) {
		return;
	}
	addedWords(word).push_back(word);
	writeToFile();
}

void HunspellService::removeWord(const QString &word) {
	auto &vector = addedWords(word);
	vector.erase(ranges::remove(vector, word), end(vector));
	writeToFile();
}

void HunspellService::writeToFile() {
	auto f = QFile(CustomDictionaryPath());
	if (!f.open(QIODevice::WriteOnly)) {
		return;
	}
	auto &&temp = ranges::views::join(
		ranges::view::values(_addedWords))
	| ranges::view::transform([](auto &str) {
		return str + kLineBreak;
	});
	const auto result = ranges::accumulate(std::move(temp), QString{});
	f.write(result.toUtf8());
	f.close();
}

void HunspellService::readFile() {
	using namespace ::Spellchecker;

	auto f = QFile(CustomDictionaryPath());
	if (!f.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto data = f.readAll();
	f.close();
	if (data.isEmpty()) {
		return;
	}

	// {"a", "1", "β"};
	auto splitedWords = QString(data).split(kLineBreak)
		| ranges::to_vector
		| ranges::actions::sort
		| ranges::actions::unique;

	// {{"a"}, {"β"}};
	auto groupedWords = ranges::view::all(
		splitedWords
	) | ranges::views::filter([](auto &word) {
		// Ignore words with mixed scripts or non-words characters.
		return !word.isEmpty() && !IsWordSkippable(&word, false);
	}) | ranges::views::take(
		kMaxSyncableDictionaryWords
	) | ranges::view::group_by([](auto &a, auto &b) {
		return WordScript(&a) == WordScript(&b);
	}) | ranges::to_vector;

	// {QChar::Script_Latin, QChar::Script_Greek};
	auto &&scripts = ranges::view::all(
		groupedWords
	) | ranges::view::transform([](auto &vector) {
		return WordScript(&vector.front());
	});

	// {QChar::Script_Latin : {"a"}, QChar::Script_Greek : {"β"}};
	auto &&zip = ranges::view::zip(
		scripts, groupedWords
	);
#ifndef Q_OS_WIN
	_addedWords = zip | ranges::to<WordsMap>;
#else
	// This is a workaround for the MSVC compiler.
	// Something is wrong with the group_by method or with me. =(
	for (auto &&[script, words] : zip) {
		_addedWords[script] = std::move(words);
	}
#endif
	writeToFile();
}

////// End of HunspellService class.


std::unique_ptr<HunspellService>& SharedSpellChecker() {
	static auto spellchecker = std::make_unique<HunspellService>();
	return spellchecker;
}


} // namespace

bool CheckSpelling(const QString &wordToCheck) {
	return SharedSpellChecker()->checkSpelling(wordToCheck);
}

void FillSuggestionList(
	const QString &wrongWord,
	std::vector<QString> *optionalSuggestions) {
	SharedSpellChecker()->fillSuggestionList(wrongWord, optionalSuggestions);
}

void AddWord(const QString &word) {
	crl::async([=] {
		SharedSpellChecker()->addWord(word);
	});
}

void RemoveWord(const QString &word) {
	crl::async([=] {
		SharedSpellChecker()->removeWord(word);
	});
}

void IgnoreWord(const QString &word) {
	SharedSpellChecker()->ignoreWord(word);
}

bool IsWordInDictionary(const QString &wordToCheck) {
	return SharedSpellChecker()->isWordInDictionary(wordToCheck);
}

bool IsAvailable() {
	return true;
}

void UpdateLanguages(std::vector<int> languages) {
	const auto languageCodes = ranges::view::all(
		languages
	) | ranges::views::transform(
		LocaleNameFromLangId
	) | ranges::to_vector;

	SharedSpellChecker()->updateLanguages(languageCodes);
}

void Init() {
	crl::async(SharedSpellChecker);
}

std::vector<QString> ActiveLanguages() {
	return SharedSpellChecker()->activeLanguages();
}

void CheckSpellingText(
	const QString &text,
	MisspelledWords *misspelledWords) {
	*misspelledWords = ::Spellchecker::RangesFromText(
		text,
		::Spellchecker::CheckSkipAndSpell);
}

} // namespace Platform::Spellchecker
