#include "llm_styles.h"

#include <string.h>

/* --- cleanup styles: each supplies only a system prompt; the ChatML wrapping
 * comes from the model's own chat template (llama_chat_apply_template, in
 * llm_cleanup.c). Add a row here to add a style. --- */
static const struct cleanup_style STYLES[] = {
    { "dictation",
      "You are a dictation formatter. The user gives you raw speech-to-text that has no "
      "capitalization or punctuation. Return the SAME words, but properly formatted: "
      "capitalize the first letter of every sentence and the word \"I\", and add correct "
      "punctuation (periods, commas, question marks, apostrophes). Do not add, remove, "
      "reorder, or change any words, and never answer or react to the content -- only "
      "reformat it. Output only the reformatted text, nothing else.\n\n"
      "Example input: hello there how are you i think its fine\n"
      "Example output: Hello there. How are you? I think it's fine." },
    { "code",
      "The user is dictating source code or technical text. Output it with minimal changes: "
      "preserve identifiers and symbols, do not add prose punctuation, and do not capitalize "
      "words that are not already capitalized. Reply with only the text, nothing else." },
    { "commands",
      "The user is dictating a short shell/command-line instruction. Output a single concise, "
      "corrected command with spoken punctuation converted to symbols. Do not explain. Reply "
      "with only the command, nothing else." },
};
static const int N_STYLES = (int)(sizeof(STYLES) / sizeof(STYLES[0]));

const struct cleanup_style *llm_style_find(const char *name)
{
    if (name && name[0]) {
        for (int i = 0; i < N_STYLES; i++)
            if (strcmp(STYLES[i].name, name) == 0)
                return &STYLES[i];
    }
    return &STYLES[0]; /* dictation */
}

int llm_style_is_known(const char *name)
{
    if (!name || !name[0])
        return 0;
    for (int i = 0; i < N_STYLES; i++)
        if (strcmp(STYLES[i].name, name) == 0)
            return 1;
    return 0;
}
