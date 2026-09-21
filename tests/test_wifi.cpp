// Wi-Fi Enterprise: упаковка логина и пароля в одну строку хранилища (CalmodWifi.h, чистая часть).
#include <cstdio>
#include <string>

#include "../overlay/src/CalmodWifi.h"

int main() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    if (!ok) {
      std::printf("FAIL %s\n", what);
      ++fails;
    }
  };
  using calmod_wifi::compose;
  using calmod_wifi::split;
  std::string u, p;

  // Круговой проход, в том числе пустой пароль, пробелы и не-ASCII.
  for (auto pr : {std::pair<const char*, const char*>{"ivan.petrov@corp.ru", "S3cret!"},
                  {"a", "b"}, {"user name", "pass word"}, {"логин", "пароль"}, {"u", ""}, {"", "p"}}) {
    const std::string s = compose(pr.first, pr.second);
    check(split(s.c_str(), u, p), "split(compose) распознан");
    check(u == pr.first && p == pr.second, "split(compose) значения");
  }

  // Обычные пароли не принимаются за Enterprise и не меняют выходные строки.
  u = "keep-u";
  p = "keep-p";
  for (const char* plain : {"", "password", "pa\x1Fss", "12345678", " "}) {
    check(!split(plain, u, p), plain);
  }
  check(!split(nullptr, u, p), "nullptr");
  check(u == "keep-u" && p == "keep-p", "не-Enterprise не портит user/pass");

  // Разделитель только в начале (нет второго) — не наша строка.
  check(!split("\x1Flogin-без-пароля", u, p), "нет второго разделителя");

  // Пароль сам может содержать разделитель после второго — берётся целиком.
  check(split("\x1Fu\x1Fp\x1Fq", u, p) && u == "u" && p == "p\x1Fq", "разделитель внутри пароля");

  // Длина: 64 + 64 + 2 укладывается в лимит хранилища (хук поднимает его до 160).
  check(compose(std::string(64, 'a'), std::string(64, 'b')).size() == 130, "максимальная длина");

  std::printf("wifi: ошибок %d\n", fails);
  return fails;
}
