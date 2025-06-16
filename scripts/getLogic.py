#!/usr/bin/env python3

import os
import time
import openai

from ollama import chat, ChatResponse

def main():
    """
    Prompts the user for:
      1) Experiment name
      2) Model choice
      3) A user prompt
    Then calls either OpenAI or local Ollama, logs to log.txt, and saves input/output to disk.
    """

    # 1) Ask for experiment name
    experiment_name = input("Enter experiment name: ").strip()
    if not experiment_name:
        raise ValueError("Experiment name cannot be empty.")

    # 2) Choose the model
    print("\nChoose the model:\n  1) chatgpt-3.5-turbo\n  2) gpt-4.1\n  3) phi3:Latest (local)\n")
    model_choice = input("Enter your choice (1/2/3): ").strip()
    if model_choice not in ["1", "2", "3"]:
        raise ValueError("Invalid choice. Must be 1, 2, or 3.")

    # 3) Ask for a user prompt
    user_prompt = input("\nEnter your prompt: ").strip()
    if not user_prompt:
        raise ValueError("User prompt cannot be empty.")

    output_dir = "output"
    os.makedirs(output_dir, exist_ok=True)
    input_filename = os.path.join(output_dir, f"{experiment_name} - input.txt")
    output_filename = os.path.join(output_dir, f"{experiment_name} - output.txt")

    # 4) Log the experiment
    log_filename = "log.txt"
    with open(log_filename, "a", encoding="utf-8") as log_file:
        timestamp = time.ctime()
        log_file.write(
            f"[{timestamp}] Experiment '{experiment_name}' started. "
            f"Using model {model_choice}. See '{experiment_name} - input/output files for details.'.\n"
        )

    # 5) Write the user prompt to <experiment name - input>.txt
    with open(input_filename, "w", encoding="utf-8") as f_in:
        f_in.write(user_prompt + "\n")

    # 6) Generate the response
    response_text = get_response(user_prompt, model_choice)

    # 7) Write the response to <experiment name - output>.txt
    with open(output_filename, "w", encoding="utf-8") as f_out:
        f_out.write(response_text + "\n")

    # 8) Notify user
    print(f"\nDone! The response has been saved to:\n  {output_filename}")
    print(f"\nThe promt has been saved to:\n  {input_filename}")


def get_response(user_prompt: str, model_choice: str) -> str:
    system_prompt = (
        "You are a lua expert writing simple automation control logic. "
        "Do not output anything else than pure lua-script. "
        "The only function allowed is cycle()."
        "This means all your code must be inside function cycle()"
        "Each cycle you read modbus registers, "
        "evaluate and execute necessary writes to the registers upon the defined conditions "
        "according to the client specifications.\n\n"
        "You know the special hooks to read\\write to modbus addresses where the first parameter "
        "is the address and the second parameter is the value written:\n"
        "modbus.readCoil(address)\n"
        "modbus.writeCoil(address, value) -- true or false\n"
        "modbus.readDiscreteInput(address)\n"
        "modbus.writeDiscreteInput(address, value) -- true or false\n"
        "modbus.readHoldingRegister(address)\n"
        "modbus.writeHoldingRegister(address, value) -- int\n"
        "modbus.readInputRegister(address)\n"
        "modbus.writeInputRegister(address, value) -- int\n\n"
        "By using these commands you are able to read and overwrite modbus registers "
        "that are read by the automation system as a modbus client."
        "Do not assume anything about the mappings. Treat them as the user instructs you."
        "Dont use formatting when outputting code. Print plain text"
    )

    if model_choice in ["1", "2"]:
        # OpenAI-based models
        openai.api_key = os.getenv("OPENAI_API_KEY")
        if not openai.api_key:
            raise ValueError("Please set the OPENAI_API_KEY environment variable for OpenAI usage.")

        # Determine which OpenAI model
        if model_choice == "1":
            openai_model = "gpt-3.5-turbo"
        else:
            openai_model = "gpt-4.1"
        response = openai.chat.completions.create(
            model=openai_model,
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_prompt},
            ],
            temperature=0.7
        )
        response_text = response.choices[0].message.content

    else:
        response: ChatResponse = chat(
            model="gemma3:12b",
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_prompt},
            ]
        )
        response_text = response.message.content

    return response_text.strip()

if __name__ == "__main__":
    main()
